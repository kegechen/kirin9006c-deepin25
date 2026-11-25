/*
 * 独立验证：UOS 的厂商 libmali（Valhall r23p0）在 deepin 25 用户态下，能否按 wlroots 的路子工作：
 *   card0 -> gbm_create_device -> eglGetPlatformDisplayEXT(GBM) -> 无 config 的 GLES 上下文(surfaceless)
 *   -> gbm_bo(SCANOUT|RENDERING) -> dmabuf -> EGLImage -> FBO 渲染 -> 回读 -> 导入 DRM AddFB2（能否扫描输出）
 *   -> 简单性能测试（全屏混合四边形）
 * 全部 dlopen，不依赖开发头文件，也不经过 glvnd。
 * 编译：gcc -O2 -o malitest malitest.c -ldl
 * 用法：malitest /path/to/libmali.so [宽 高]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void *EGLDisplay, *EGLContext, *EGLImage, *EGLConfig, *EGLSurface;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;
typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLubyte;
struct gbm_device;
struct gbm_bo;
union gbm_bo_handle { void *ptr; int32_t s32; uint32_t u32; int64_t s64; uint64_t u64; };

#define EGL_PLATFORM_GBM_KHR 0x31D7
#define EGL_EXTENSIONS 0x3055
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_CLIENT_APIS 0x308D
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NONE 0x3038
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GBM_FORMAT_XRGB8888 0x34325258u
#define GBM_FORMAT_ARGB8888 0x34325241u
#define GBM_BO_USE_SCANOUT 1
#define GBM_BO_USE_RENDERING 4

#define LOAD(lib, name) \
    do { \
        *(void **) &name = dlsym(lib, #name); \
        if (!name) { fprintf(stderr, "缺少符号 %s\n", #name); } \
    } while (0)

static struct gbm_device *(*gbm_create_device)(int);
static const char *(*gbm_device_get_backend_name)(struct gbm_device *);
static struct gbm_bo *(*gbm_bo_create)(struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);
static int (*gbm_bo_get_fd)(struct gbm_bo *);
static uint32_t (*gbm_bo_get_stride)(struct gbm_bo *);
static uint64_t (*gbm_bo_get_modifier)(struct gbm_bo *);
static union gbm_bo_handle (*gbm_bo_get_handle)(struct gbm_bo *);
static EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static const char *(*eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*eglBindAPI)(EGLenum);
static EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLint (*eglGetError)(void);
static void *(*eglGetProcAddress)(const char *);
static EGLImage (*eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum, void *, const EGLint *);
static const GLubyte *(*glGetString)(GLenum);
static void (*glGenFramebuffers)(GLsizei, GLuint *);
static void (*glBindFramebuffer)(GLenum, GLuint);
static void (*glGenRenderbuffers)(GLsizei, GLuint *);
static void (*glBindRenderbuffer)(GLenum, GLuint);
static void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*glCheckFramebufferStatus)(GLenum);
static void (*glEGLImageTargetRenderbufferStorageOES)(GLenum, void *);
static void (*glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*glClear)(GLbitfield);
static void (*glFinish)(void);
static void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
static GLuint (*glCreateShader)(GLenum);
static void (*glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (*glCompileShader)(GLuint);
static void (*glGetShaderiv)(GLuint, GLenum, GLint *);
static GLuint (*glCreateProgram)(void);
static void (*glAttachShader)(GLuint, GLuint);
static void (*glLinkProgram)(GLuint);
static void (*glGetProgramiv)(GLuint, GLenum, GLint *);
static void (*glUseProgram)(GLuint);
static void (*glBindAttribLocation)(GLuint, GLuint, const char *);
static void (*glVertexAttribPointer)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void *);
static void (*glEnableVertexAttribArray)(GLuint);
static void (*glDrawArrays)(GLenum, GLint, GLsizei);
static void (*glEnable)(GLenum);
static void (*glBlendFunc)(GLenum, GLenum);
static GLint (*glGetUniformLocation)(GLuint, const char *);
static void (*glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static int (*drmPrimeFDToHandle)(int, int, uint32_t *);
static int (*drmModeAddFB2)(int, uint32_t, uint32_t, uint32_t, const uint32_t *, const uint32_t *, const uint32_t *, uint32_t *, uint32_t);
static int (*drmModeRmFB)(int, uint32_t);

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int has_ext(const char *list, const char *e)
{
    size_t n = strlen(e);
    for (const char *p = list; p && (p = strstr(p, e)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "libmali.so.0";
    uint32_t    W = argc > 3 ? atoi(argv[2]) : 1920, H = argc > 3 ? atoi(argv[3]) : 1080;
    void       *m = dlopen(path, RTLD_NOW | RTLD_GLOBAL), *drm;
    if (!m) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); return 1; }
    drm = dlopen("libdrm.so.2", RTLD_NOW);

    LOAD(m, gbm_create_device); LOAD(m, gbm_device_get_backend_name); LOAD(m, gbm_bo_create);
    LOAD(m, gbm_bo_get_fd); LOAD(m, gbm_bo_get_stride); LOAD(m, gbm_bo_get_modifier); LOAD(m, gbm_bo_get_handle);
    LOAD(m, eglGetPlatformDisplayEXT); LOAD(m, eglInitialize); LOAD(m, eglQueryString); LOAD(m, eglBindAPI);
    LOAD(m, eglCreateContext); LOAD(m, eglMakeCurrent); LOAD(m, eglGetError); LOAD(m, eglGetProcAddress);
    LOAD(m, glGetString); LOAD(m, glGenFramebuffers); LOAD(m, glBindFramebuffer); LOAD(m, glGenRenderbuffers);
    LOAD(m, glBindRenderbuffer); LOAD(m, glFramebufferRenderbuffer); LOAD(m, glCheckFramebufferStatus);
    LOAD(m, glClearColor); LOAD(m, glClear); LOAD(m, glFinish); LOAD(m, glReadPixels); LOAD(m, glViewport);
    LOAD(m, glCreateShader); LOAD(m, glShaderSource); LOAD(m, glCompileShader); LOAD(m, glGetShaderiv);
    LOAD(m, glCreateProgram); LOAD(m, glAttachShader); LOAD(m, glLinkProgram); LOAD(m, glGetProgramiv);
    LOAD(m, glUseProgram); LOAD(m, glBindAttribLocation); LOAD(m, glVertexAttribPointer);
    LOAD(m, glEnableVertexAttribArray); LOAD(m, glDrawArrays); LOAD(m, glEnable); LOAD(m, glBlendFunc);
    LOAD(m, glGetUniformLocation); LOAD(m, glUniform4f);
    LOAD(drm, drmPrimeFDToHandle); LOAD(drm, drmModeAddFB2); LOAD(drm, drmModeRmFB);
    if (!eglGetPlatformDisplayEXT) {
        *(void **) &eglGetPlatformDisplayEXT = dlsym(m, "eglGetPlatformDisplay");
        if (!eglGetPlatformDisplayEXT && eglGetProcAddress)
            *(void **) &eglGetPlatformDisplayEXT = eglGetProcAddress("eglGetPlatformDisplayEXT");
    }

    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card0"); return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    printf("gbm_create_device(card0) = %p backend=%s\n", (void *) gbm,
           gbm && gbm_device_get_backend_name ? gbm_device_get_backend_name(gbm) : "?");
    if (!gbm) return 1;

    printf("EGL client exts: %s\n", eglQueryString(NULL, EGL_EXTENSIONS));
    EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint     maj = 0, min = 0;
    if (!dpy || !eglInitialize(dpy, &maj, &min)) { printf("eglInitialize 失败 0x%x\n", eglGetError()); return 1; }
    printf("EGL %d.%d vendor=%s version=%s apis=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR),
           eglQueryString(dpy, EGL_VERSION), eglQueryString(dpy, EGL_CLIENT_APIS));
    printf("EGL display exts: %s\n", eglQueryString(dpy, EGL_EXTENSIONS));

    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint     cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx     = eglCreateContext(dpy, NULL, NULL, cattr);
    if (!ctx) { printf("eglCreateContext(no config) 失败 0x%x\n", eglGetError()); return 1; }
    if (!eglMakeCurrent(dpy, NULL, NULL, ctx)) { printf("eglMakeCurrent(surfaceless) 失败 0x%x\n", eglGetError()); return 1; }
    const char *glext = (const char *) glGetString(GL_EXTENSIONS);
    printf("GL vendor=%s renderer=%s version=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    printf("GL_EXT_unpack_subimage=%d GL_EXT_texture_format_BGRA8888=%d GL_OES_EGL_image_external=%d GL_EXT_read_format_bgra=%d\n",
           has_ext(glext, "GL_EXT_unpack_subimage"), has_ext(glext, "GL_EXT_texture_format_BGRA8888"),
           has_ext(glext, "GL_OES_EGL_image_external"), has_ext(glext, "GL_EXT_read_format_bgra"));

    *(void **) &eglCreateImageKHR                      = eglGetProcAddress("eglCreateImageKHR");
    *(void **) &glEGLImageTargetRenderbufferStorageOES = eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");

    for (int pass = 0; pass < 2; pass++) {
        uint32_t       fmt = pass ? GBM_FORMAT_ARGB8888 : GBM_FORMAT_XRGB8888;
        struct gbm_bo *bo  = gbm_bo_create(gbm, W, H, fmt, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!bo) { printf("[%s] gbm_bo_create 失败\n", pass ? "AR24" : "XR24"); continue; }
        int      dfd    = gbm_bo_get_fd(bo);
        uint32_t stride = gbm_bo_get_stride(bo);
        printf("[%s] bo=%p dmabuf fd=%d stride=%u modifier=0x%llx handle=%u\n", pass ? "AR24" : "XR24", (void *) bo, dfd,
               stride, gbm_bo_get_modifier ? (unsigned long long) gbm_bo_get_modifier(bo) : 0ULL, gbm_bo_get_handle(bo).u32);
        EGLint   iattr[] = {EGL_WIDTH, (EGLint) W, EGL_HEIGHT, (EGLint) H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint) fmt,
                            EGL_DMA_BUF_PLANE0_FD_EXT, dfd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint) stride, EGL_NONE};
        EGLImage img = eglCreateImageKHR(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, iattr);
        printf("  eglCreateImageKHR(dmabuf) = %p err=0x%x\n", img, img ? 0x3000 : eglGetError());
        if (img) {
            GLuint fb, rb;
            glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, img);
            glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            printf("  FBO status=0x%x (0x8cd5=完整)\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
            glViewport(0, 0, W, H);
            glClearColor(0.25f, 0.5f, 0.75f, 1.0f); glClear(GL_COLOR_BUFFER_BIT); glFinish();
            unsigned char px[4] = {0};
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("  清屏回读 RGBA=%u,%u,%u,%u（期望约 64,128,191,255）\n", px[0], px[1], px[2], px[3]);

            if (pass == 0) {
                static const char *vs = "attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }";
                static const char *fs = "precision mediump float; uniform vec4 c; void main(){ gl_FragColor = c; }";
                GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), pr = glCreateProgram();
                GLint  ok1 = 0, ok2 = 0, ok3 = 0;
                glShaderSource(v, 1, &vs, NULL); glCompileShader(v); glGetShaderiv(v, GL_COMPILE_STATUS, &ok1);
                glShaderSource(f, 1, &fs, NULL); glCompileShader(f); glGetShaderiv(f, GL_COMPILE_STATUS, &ok2);
                glAttachShader(pr, v); glAttachShader(pr, f); glBindAttribLocation(pr, 0, "p"); glLinkProgram(pr);
                glGetProgramiv(pr, GL_LINK_STATUS, &ok3);
                printf("  着色器编译/链接: %d %d %d\n", ok1, ok2, ok3);
                static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
                glUseProgram(pr);
                glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad); glEnableVertexAttribArray(0);
                glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                GLint  cl = glGetUniformLocation(pr, "c");
                double t0 = now();
                int    frames = 120, layers = 8;
                for (int i = 0; i < frames; i++) {
                    glClear(GL_COLOR_BUFFER_BIT);
                    for (int k = 0; k < layers; k++) {
                        glUniform4f(cl, (k & 1) * 1.0f, (k & 2) * 0.5f, (k & 4) * 0.25f, 0.3f);
                        glDrawArrays(GL_TRIANGLES, 0, 6);
                    }
                    glFinish();
                }
                double dt = now() - t0;
                printf("  性能：%d 帧 × %d 层全屏混合 %ux%u，共 %.3f s，%.2f ms/帧，约 %.0f FPS\n", frames, layers, W, H, dt,
                       dt * 1000 / frames, frames / dt);
                glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                printf("  混合结果回读 RGBA=%u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
            }
        }
        /* 能否作为扫描输出：dmabuf 导回 hisi-drm 建 framebuffer */
        uint32_t h = 0, fbid = 0, hs[4] = {0}, ps[4] = {stride}, os[4] = {0};
        int      r = drmPrimeFDToHandle(fd, dfd, &h);
        printf("  drmPrimeFDToHandle=%d handle=%u\n", r, h);
        if (r == 0) {
            hs[0] = h;
            r     = drmModeAddFB2(fd, W, H, fmt, hs, ps, os, &fbid, 0);
            printf("  drmModeAddFB2=%d fb=%u（0=可作扫描输出）\n", r, fbid);
            if (r == 0) drmModeRmFB(fd, fbid);
        }
        close(dfd);
    }
    return 0;
}
