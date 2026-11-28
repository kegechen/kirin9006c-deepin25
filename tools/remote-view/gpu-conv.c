// gpu-conv：在 Mali GPU 上把 screencopy 的 XRGB 画面缩放并转成 I420，给 rv-cap 用
//
// 为什么：dumb buffer 是写合并内存，CPU 读只有约 950 MB/s。全分辨率 XRGB 一帧 12.4 MB，光读就要 13 ms，
// 之后还要在 CPU 上缩放、转换。先在 GPU 上转成 I420（1.5 字节/像素）再读，要读的数据少 62%，
// 缩放和转换也不占 CPU。
//
// 做法：直接 dlopen 厂商 libmali（glvnd 垫片只认领 Wayland 平台，这里用的是 GBM 平台），在 card0 上建
// GBM 平台的 EGL display 和无表面的 GLES2 上下文。screencopy 的 dumb buffer 作为纹理导入；另建一块
// dumb buffer 当渲染目标，按 ARGB8888 看待，宽 out_w/4、高 out_h*3/2，一个像素装 4 个字节：
//   行 [0, out_h)                 Y 平面，每行 out_w 字节
//   行 [out_h, out_h*3/2) 的左半   U 平面，每行 out_w/2 字节
//   行 [out_h, out_h*3/2) 的右半   V 平面
// 三个平面的行距都等于目标的 pitch，读回时按行拷进 x264 的图。
// 缩放用若干个双线性采样点近似面积平均；不缩放时亮度正好取像素中心，色度正好是 2×2 平均。
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include "gpu-conv.h"

#define LIBMALI "/usr/local/lib/hisi-mali/libmali.so.0"
#define LOG(...) fprintf(stderr, "rv-cap: " __VA_ARGS__)

// libmali 按 UOS 魔改的 libwayland-client 编译，依赖这三个上游没有的符号，而且是 BIND_NOW，
// 缺了 dlopen 直接失败。这里只用 GBM 平台，libmali 不会走 Wayland 代码，给空实现即可；
// Makefile 用 --export-dynamic-symbol 把它们导出给 libmali 解析。
struct wl_proxy;
void wl_proxy_ref(struct wl_proxy *p) { (void)p; }
void wl_proxy_unref(struct wl_proxy *p) { (void)p; }
int wl_proxy_get_refcount(struct wl_proxy *p) { (void)p; return 1; }

// 不依赖 EGL/GLES 头文件（系统里的是 Mesa/glvnd 的），需要的类型和常量自己定义
typedef void *EGLDisplay, *EGLContext, *EGLImage, *EGLConfig, *EGLSurface;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;
typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte, GLboolean;
typedef char GLchar;

#define EGL_PLATFORM_GBM_KHR 0x31D7
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define GL_NO_ERROR 0
#define GL_TRIANGLE_STRIP 0x0005
#define GL_DITHER 0x0BD0
#define GL_BLEND 0x0BE2
#define GL_TEXTURE_2D 0x0DE1
#define GL_FLOAT 0x1406
#define GL_RENDERER 0x1F01
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_TEXTURE_EXTERNAL_OES 0x8D65

static void *(*eglGetProcAddress)(const char *);
static struct gbm_device *(*gbm_create_device)(int);
static EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*eglBindAPI)(EGLenum);
static EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLint (*eglGetError)(void);
static EGLImage (*eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum, void *, const EGLint *);
static void (*glEGLImageTargetTexture2DOES)(GLenum, void *);
static void (*glEGLImageTargetRenderbufferStorageOES)(GLenum, void *);
static const GLubyte *(*glGetString)(GLenum);
static GLenum (*glGetError)(void);
static void (*glDisable)(GLenum);
static void (*glActiveTexture)(GLenum);
static void (*glGenTextures)(GLsizei, GLuint *);
static void (*glBindTexture)(GLenum, GLuint);
static void (*glTexParameteri)(GLenum, GLenum, GLint);
static void (*glGenFramebuffers)(GLsizei, GLuint *);
static void (*glBindFramebuffer)(GLenum, GLuint);
static void (*glGenRenderbuffers)(GLsizei, GLuint *);
static void (*glBindRenderbuffer)(GLenum, GLuint);
static void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*glCheckFramebufferStatus)(GLenum);
static void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
static GLuint (*glCreateShader)(GLenum);
static void (*glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static void (*glCompileShader)(GLuint);
static void (*glGetShaderiv)(GLuint, GLenum, GLint *);
static void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static GLuint (*glCreateProgram)(void);
static void (*glAttachShader)(GLuint, GLuint);
static void (*glBindAttribLocation)(GLuint, GLuint, const GLchar *);
static void (*glLinkProgram)(GLuint);
static void (*glGetProgramiv)(GLuint, GLenum, GLint *);
static void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static void (*glUseProgram)(GLuint);
static GLint (*glGetUniformLocation)(GLuint, const GLchar *);
static void (*glUniform1i)(GLint, GLint);
static void (*glUniform1f)(GLint, GLfloat);
static void (*glUniform2f)(GLint, GLfloat, GLfloat);
static void (*glUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
static void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*glEnableVertexAttribArray)(GLuint);
static void (*glDrawArrays)(GLenum, GLint, GLsizei);
static void (*glFinish)(void);

#define SYM(name) {#name, (void **)&name}
static const struct { const char *name; void **ptr; } syms[] = {
	SYM(gbm_create_device), SYM(eglInitialize), SYM(eglBindAPI), SYM(eglCreateContext),
	SYM(eglMakeCurrent), SYM(eglGetError), SYM(eglCreateImageKHR), SYM(glEGLImageTargetTexture2DOES),
	SYM(glEGLImageTargetRenderbufferStorageOES), SYM(glGetString), SYM(glGetError), SYM(glDisable),
	SYM(glActiveTexture), SYM(glGenTextures), SYM(glBindTexture), SYM(glTexParameteri),
	SYM(glGenFramebuffers), SYM(glBindFramebuffer), SYM(glGenRenderbuffers), SYM(glBindRenderbuffer),
	SYM(glFramebufferRenderbuffer), SYM(glCheckFramebufferStatus), SYM(glViewport), SYM(glCreateShader),
	SYM(glShaderSource), SYM(glCompileShader), SYM(glGetShaderiv), SYM(glGetShaderInfoLog),
	SYM(glCreateProgram), SYM(glAttachShader), SYM(glBindAttribLocation), SYM(glLinkProgram),
	SYM(glGetProgramiv), SYM(glGetProgramInfoLog), SYM(glUseProgram), SYM(glGetUniformLocation),
	SYM(glUniform1i), SYM(glUniform1f), SYM(glUniform2f), SYM(glUniform3f), SYM(glVertexAttribPointer),
	SYM(glEnableVertexAttribArray), SYM(glDrawArrays), SYM(glFinish),
};

// BT.709 有限范围，已乘上 219/255（亮度）或 224/255（色度）
#define KS_Y (219.0f / 255.0f)
#define KS_C (224.0f / 255.0f)
static const float KY[3] = {0.2126f * KS_Y, 0.7152f * KS_Y, 0.0722f * KS_Y};
static const float KU[3] = {-0.114572f * KS_C, -0.385428f * KS_C, 0.5f * KS_C};
static const float KV[3] = {0.5f * KS_C, -0.454153f * KS_C, -0.045847f * KS_C};

static int o_w, o_h, d_w4, d_h;
static GLuint fbo, prog_y, prog_c;
static GLint c_k, c_org;
static int dst_fd = -1;
static uint8_t *dst_map;
static size_t dst_size;
static uint32_t dst_pitch;

static const char *vs_src =
	"attribute vec2 p;\n"
	"void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";

// 每个输出像素（4 字节里的一个）= 它在源图里覆盖的矩形 [o, o + fp) 的平均，用 NX×NY 个双线性采样近似。
// gl_FragCoord 的第 0 行就是缓冲区内存里的第 0 行，纹理 t=0 也是源内存第 0 行，所以不用翻转。
static const char *fs_fmt =
	"#define NX %d\n"
	"#define NY %d\n"
	"%s"
	"precision highp float;\n"
	"uniform %s src;\n"
	"uniform vec2 inv_src;\n" // 1/源尺寸
	"uniform vec2 fp;\n"      // 一个输出像素在源图里占多大（源像素）
	"uniform vec2 org;\n"     // 本次绘制区域的左下角（窗口坐标）
	"uniform vec3 K;\n"
	"uniform float off;\n"
	"float px(float x, float y) {\n"
	"	vec2 o = vec2(x, y) * fp;\n"
	"	vec3 acc = vec3(0.0);\n"
	"	for (int j = 0; j < NY; j++)\n"
	"		for (int i = 0; i < NX; i++)\n"
	"			acc += texture2D(src, (o + fp * (vec2(float(i), float(j)) + 0.5) / vec2(float(NX), float(NY))) * inv_src).rgb;\n"
	"	return dot(K, acc) / float(NX * NY) + off;\n"
	"}\n"
	"void main() {\n"
	"	vec2 f = floor(gl_FragCoord.xy - org);\n"
	"	float x = f.x * 4.0;\n"
	"	vec4 v = vec4(px(x, f.y), px(x + 1.0, f.y), px(x + 2.0, f.y), px(x + 3.0, f.y));\n"
	"	gl_FragColor = v.bgra;\n" // 目标是 ARGB8888，内存字节顺序 B G R A
	"}\n";

static GLuint compile(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		LOG("GPU 着色器编译失败：%s\n", log);
		return 0;
	}
	return s;
}

// 一个输出像素占 fp 个源像素时每个方向的采样点数：每个双线性采样覆盖约 2 个源像素
static int taps_for(double fp) {
	int n = (int)ceil(fp / 2 - 1e-6);
	return n < 1 ? 1 : n;
}

static GLuint make_program(bool external, double fpx, double fpy, int src_w, int src_h) {
	char fs[4096];
	snprintf(fs, sizeof fs, fs_fmt, taps_for(fpx), taps_for(fpy),
	         external ? "#extension GL_OES_EGL_image_external : require\n" : "",
	         external ? "samplerExternalOES" : "sampler2D");
	GLuint v = compile(GL_VERTEX_SHADER, vs_src), f = compile(GL_FRAGMENT_SHADER, fs);
	if (!v || !f) return 0;
	GLuint p = glCreateProgram();
	GLint ok = 0;
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetProgramInfoLog(p, sizeof log, NULL, log);
		LOG("GPU 着色器链接失败：%s\n", log);
		return 0;
	}
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "src"), 0);
	glUniform2f(glGetUniformLocation(p, "inv_src"), 1.0f / src_w, 1.0f / src_h);
	glUniform2f(glGetUniformLocation(p, "fp"), fpx, fpy);
	return p;
}

#define FAIL(...) do { LOG("GPU 转换不可用，改用 CPU：" __VA_ARGS__); return -1; } while (0)

int gpu_init(int drm_fd, int src_fd, uint32_t src_fourcc, int src_w, int src_h, uint32_t src_pitch,
             int out_w, int out_h) {
	if (out_w % 8 || out_h % 2) FAIL("输出尺寸 %dx%d 宽要是 8 的倍数、高要是偶数\n", out_w, out_h);
	o_w = out_w; o_h = out_h; d_w4 = out_w / 4; d_h = out_h * 3 / 2;

	void *lib = dlopen(LIBMALI, RTLD_NOW | RTLD_LOCAL);
	if (!lib) FAIL("加载 libmali 失败（%s）\n", dlerror());
	*(void **)&eglGetProcAddress = dlsym(lib, "eglGetProcAddress");
	*(void **)&eglGetPlatformDisplayEXT = dlsym(lib, "eglGetPlatformDisplayEXT");
	if (!eglGetPlatformDisplayEXT) // 只有 EGL 1.5 版也行：第三个参数传 NULL，EGLAttrib/EGLint 无所谓
		*(void **)&eglGetPlatformDisplayEXT = dlsym(lib, "eglGetPlatformDisplay");
	if (!eglGetPlatformDisplayEXT && eglGetProcAddress) // libmali 不导出，只能这样取
		*(void **)&eglGetPlatformDisplayEXT = eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!eglGetProcAddress || !eglGetPlatformDisplayEXT) FAIL("libmali 缺 EGL 入口\n");
	for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
		*syms[i].ptr = dlsym(lib, syms[i].name);
		if (!*syms[i].ptr) *syms[i].ptr = eglGetProcAddress(syms[i].name);
		if (!*syms[i].ptr) FAIL("libmali 缺 %s\n", syms[i].name);
	}

	struct gbm_device *gbm = gbm_create_device(drm_fd);
	if (!gbm) FAIL("gbm_create_device 失败\n");
	EGLDisplay edpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	EGLint maj, min;
	if (!edpy || !eglInitialize(edpy, &maj, &min)) FAIL("eglInitialize 失败 0x%x\n", eglGetError());
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLContext ctx = eglCreateContext(edpy, NULL, NULL, cattr);
	if (!ctx || !eglMakeCurrent(edpy, NULL, NULL, ctx)) FAIL("建 GLES 上下文失败 0x%x\n", eglGetError());

	// 源：screencopy 的 dumb buffer（线性；不带 modifier 就是隐式，和给合成器的一致）
	const EGLint sattr[] = {EGL_WIDTH, src_w, EGL_HEIGHT, src_h, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)src_fourcc,
	                        EGL_DMA_BUF_PLANE0_FD_EXT, src_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
	                        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)src_pitch, EGL_NONE};
	EGLImage simg = eglCreateImageKHR(edpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, sattr);
	if (!simg) FAIL("导入源缓冲区失败 0x%x\n", eglGetError());
	GLuint tex;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, simg);
	GLenum target = GL_TEXTURE_2D;
	if (glGetError() != GL_NO_ERROR) { // 有的驱动只让 dmabuf 当外部纹理用
		target = GL_TEXTURE_EXTERNAL_OES;
		glBindTexture(target, tex);
		glEGLImageTargetTexture2DOES(target, simg);
		if (glGetError() != GL_NO_ERROR) FAIL("源缓冲区绑不上纹理\n");
	}
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// 目标：card0 上的 dumb buffer，GPU 写、CPU 读。Mali 渲染目标的行距要 64 字节对齐，
	// 否则导入报 EGL_BAD_ALLOC，所以分配宽度取整到 16 像素（EGLImage 仍按 d_w4 宽）
	struct drm_mode_create_dumb c = {.width = (d_w4 + 15) & ~15, .height = d_h, .bpp = 32};
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) FAIL("建目标 dumb buffer 失败\n");
	if (drmPrimeHandleToFD(drm_fd, c.handle, DRM_CLOEXEC | DRM_RDWR, &dst_fd)) FAIL("导出目标缓冲区失败\n");
	struct drm_mode_map_dumb m = {.handle = c.handle};
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) FAIL("MAP_DUMB 失败\n");
	dst_size = c.size;
	dst_pitch = c.pitch;
	dst_map = mmap(NULL, dst_size, PROT_READ, MAP_SHARED, drm_fd, m.offset);
	if (dst_map == MAP_FAILED) FAIL("mmap 目标缓冲区失败\n");
	const EGLint dattr[] = {EGL_WIDTH, d_w4, EGL_HEIGHT, d_h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
	                        EGL_DMA_BUF_PLANE0_FD_EXT, dst_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
	                        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)dst_pitch, EGL_NONE};
	EGLImage dimg = eglCreateImageKHR(edpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, dattr);
	if (!dimg) FAIL("导入目标缓冲区失败 0x%x\n", eglGetError());
	GLuint rb;
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, dimg);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (st != GL_FRAMEBUFFER_COMPLETE) FAIL("目标 FBO 不完整 0x%x\n", st);

	bool ext = target == GL_TEXTURE_EXTERNAL_OES;
	double sx = (double)src_w / out_w, sy = (double)src_h / out_h;
	prog_y = make_program(ext, sx, sy, src_w, src_h);
	prog_c = make_program(ext, 2 * sx, 2 * sy, src_w, src_h);
	if (!prog_y || !prog_c) FAIL("着色器不可用\n");
	glUseProgram(prog_y);
	glUniform3f(glGetUniformLocation(prog_y, "K"), KY[0], KY[1], KY[2]);
	glUniform1f(glGetUniformLocation(prog_y, "off"), 16.0f / 255.0f);
	glUniform2f(glGetUniformLocation(prog_y, "org"), 0, 0);
	glUseProgram(prog_c);
	glUniform1f(glGetUniformLocation(prog_c, "off"), 128.0f / 255.0f);
	c_k = glGetUniformLocation(prog_c, "K");
	c_org = glGetUniformLocation(prog_c, "org");

	static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad);
	glEnableVertexAttribArray(0);
	glDisable(GL_BLEND);
	glDisable(GL_DITHER); // 默认开着，会给 8 位输出加抖动
	if (glGetError() != GL_NO_ERROR) FAIL("GL 初始化出错\n");

	LOG("GPU 转换：%s，%s纹理，亮度 %dx%d / 色度 %dx%d 个采样点，目标行距 %u\n", glGetString(GL_RENDERER),
	    ext ? "外部" : "2D ", taps_for(sx), taps_for(sy), taps_for(2 * sx), taps_for(2 * sy), dst_pitch);
	return 0;
}

void gpu_convert(void) {
	glUseProgram(prog_y);
	glViewport(0, 0, d_w4, o_h);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(prog_c);
	glUniform3f(c_k, KU[0], KU[1], KU[2]);
	glUniform2f(c_org, 0, o_h);
	glViewport(0, o_h, d_w4 / 2, o_h / 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUniform3f(c_k, KV[0], KV[1], KV[2]);
	glUniform2f(c_org, d_w4 / 2, o_h);
	glViewport(d_w4 / 2, o_h, d_w4 / 2, o_h / 2);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
}

void gpu_readout(uint8_t *y, int ys, uint8_t *u, int us, uint8_t *v, int vs) {
	struct dma_buf_sync s = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
	ioctl(dst_fd, DMA_BUF_IOCTL_SYNC, &s);
	const uint8_t *p = dst_map;
	if (ys == (int)dst_pitch) {
		memcpy(y, p, (size_t)dst_pitch * o_h);
	} else {
		for (int r = 0; r < o_h; r++)
			memcpy(y + (size_t)r * ys, p + (size_t)r * dst_pitch, o_w);
	}
	p += (size_t)dst_pitch * o_h;
	for (int r = 0; r < o_h / 2; r++, p += dst_pitch) {
		memcpy(u + (size_t)r * us, p, o_w / 2);
		memcpy(v + (size_t)r * vs, p + o_w / 2, o_w / 2);
	}
	s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(dst_fd, DMA_BUF_IOCTL_SYNC, &s);
}
