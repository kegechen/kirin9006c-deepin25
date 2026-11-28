// rv-cap：treeland 屏幕采集 + H.264 编码，给 rv-server.py 用
//
// 采集走 wlr-screencopy 的 dmabuf 路径（shm 路径在这台机器上每帧约 750ms，见 docs/remote-view.md）：
//   1. 在 /dev/dri/card0 上建 dumb buffer（renderD128 只是显示控制器）
//   2. 5.4 内核导出 PRIME 需要 DRM 认证：drmGetMagic 后经 mali_buffer_sharing.auth 让 treeland 代为 drmAuthMagic
//   3. 用隐式 modifier（INVALID）建 wl_buffer；treeland 不宣告 LINEAR
// 之后用 copy_with_damage，画面不变就不出帧。每帧：
//   默认：GPU 缩放 + 转 I420（gpu-conv.c）→ CPU 读回 I420 → x264 zerolatency
//   -C 或 GPU 初始化失败：CPU 读回 XRGB → CPU 缩放 + 转 I420 → x264
//
// 输出（stdout）：包头 8 字节 [type u8][flags u8][0 u16][len u32 大端] + 数据
//   type 1 = 配置 JSON；type 2 = 一帧 H.264 Annex B，flags bit0 = 关键帧
//   type 3 = 状态提示（比如等亮屏），页面显示后不自动消失
// 输入（stdin）：'k' 立即补一个 IDR；EOF 退出
//
// 用法：rv-cap [-w 宽] [-h 高] [-r 最大帧率] [-q crf] [-t 线程] [-n 不叠加光标] [-C 不用 GPU 转换]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <omp.h>
#include <wayland-client.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <x264.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "gpu-conv.h"

#define LOG(...) fprintf(stderr, "rv-cap: " __VA_ARGS__)

static void die(const char *msg) {
	LOG("%s\n", msg);
	exit(2);
}

static double now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* 输出 */
static void write_all(const void *p, size_t n) {
	const uint8_t *b = p;
	while (n) {
		ssize_t r = write(1, b, n);
		if (r < 0) {
			if (errno == EINTR) continue;
			exit(0); // 对端关了管道
		}
		b += r; n -= r;
	}
}
static void send_pkt(uint8_t type, uint8_t flags, const void *data, uint32_t len) {
	uint8_t h[8] = {type, flags, 0, 0, len >> 24, len >> 16, len >> 8, len};
	write_all(h, 8);
	write_all(data, len);
}

/* Wayland */
static struct wl_display *dpy;
static struct wl_output *output;
static struct zwlr_screencopy_manager_v1 *scm;
static struct zwp_linux_dmabuf_v1 *dmabuf;

// mali_buffer_sharing（treeland 由 libtl-malibuf 提供）：只用 auth 请求（opcode 1）
static const struct wl_interface *mbs_types[] = {&wl_buffer_interface, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
static const struct wl_message mbs_requests[] = {{"create_buffer", "niiuuuuh", mbs_types}, {"auth", "u", mbs_types + 1}};
static const struct wl_message mbs_events[] = {{"alloc_device", "s", mbs_types + 1}};
static const struct wl_interface mbs_interface = {"mali_buffer_sharing", 5, 2, mbs_requests, 1, mbs_events};
static struct wl_proxy *mbs;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		scm = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, ver < 3 ? ver : 3);
	else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name))
		dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
	else if (!strcmp(iface, "mali_buffer_sharing"))
		mbs = wl_registry_bind(r, name, &mbs_interface, ver < 5 ? ver : 5);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

static struct {
	uint32_t dma_fmt, dma_w, dma_h;
	bool has_dma, done, ready, failed;
} fs;

static void f_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h, uint32_t s) {}
static void f_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fl) {}
static void f_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c) { fs.ready = true; }
static void f_failed(void *d, struct zwlr_screencopy_frame_v1 *f) { fs.failed = true; }
static void f_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {}
static void f_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h) {
	fs.has_dma = true; fs.dma_fmt = fmt; fs.dma_w = w; fs.dma_h = h;
}
static void f_done(void *d, struct zwlr_screencopy_frame_v1 *f) { fs.done = true; }
static const struct zwlr_screencopy_frame_v1_listener f_l = {
	f_buffer, f_flags, f_ready, f_failed, f_damage, f_dmabuf, f_done};

/* 参数与状态 */
static int src_w, src_h, out_w, out_h;
static int max_fps = 30, crf = 23, nthreads = 4;
static bool overlay_cursor = true;
static bool force_cpu;        // -C：不用 GPU 转换
static bool use_gpu;          // GPU 转换初始化成功

static int drm_fd = -1;
static struct wl_buffer *wlbuf;
static uint8_t *map;          // dumb buffer 映射（写合并内存，读一遍约 13ms）
static size_t map_size;
static uint32_t map_stride;
static int prime_fd = -1;
static uint8_t *frame;        // 读回到普通内存的 BGRX
static uint16_t *tmp_h;       // 横向缩放中间结果 src_h × out_w × 3
static uint8_t *scaled;       // 缩放后的 BGRX out_w × out_h

static bool swap_rb;          // 源是 XBGR（R 在低字节）时为 true；treeland 给的是 XRGB

// 编码在单独线程里做，两张图轮换：主线程往一张里转换，编码线程编另一张
static x264_t *enc;
static x264_picture_t pics[2];
static pthread_mutex_t enc_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t enc_cv = PTHREAD_COND_INITIALIZER;
static int enc_pending = -1;  // 等编码的图
static int enc_using = -1;    // 正在编码的图
static int last_filled = -1;  // 最近一次交给编码线程的图
static bool idr_next;
static double st_enc_sum;     // 编码耗时统计（受 enc_mu 保护）
static int st_enc_n;

/* 面积平均缩放 */
typedef struct { int n, idx[4], w[4]; } taps_t;
static taps_t *taps_x, *taps_y;

static taps_t *make_taps(int S, int D) {
	taps_t *t = calloc(D, sizeof *t);
	double r = (double)S / D;
	for (int i = 0; i < D; i++) {
		double a = i * r, b = (i + 1) * r;
		int sum = 0, best = 0;
		for (int j = (int)floor(a); j < S && j < b && t[i].n < 4; j++) {
			double lo = fmax(a, j), hi = fmin(b, j + 1);
			int w = (int)lround((hi - lo) / r * 256);
			if (w <= 0) continue;
			t[i].idx[t[i].n] = j; t[i].w[t[i].n] = w;
			if (w > t[i].w[best]) best = t[i].n;
			sum += w; t[i].n++;
		}
		t[i].w[best] += 256 - sum; // 舍入误差补到最大的那一项，保证权重和为 256
	}
	return t;
}

static void scale_bgrx(void) {
	#pragma omp parallel for num_threads(nthreads) schedule(static)
	for (int y = 0; y < src_h; y++) {
		const uint8_t *s = frame + (size_t)y * src_w * 4;
		uint16_t *d = tmp_h + (size_t)y * out_w * 3;
		for (int x = 0; x < out_w; x++) {
			const taps_t *t = &taps_x[x];
			int b = 0, g = 0, r = 0;
			for (int k = 0; k < t->n; k++) {
				const uint8_t *p = s + t->idx[k] * 4;
				b += p[0] * t->w[k]; g += p[1] * t->w[k]; r += p[2] * t->w[k];
			}
			d[x * 3] = b; d[x * 3 + 1] = g; d[x * 3 + 2] = r;
		}
	}
	#pragma omp parallel for num_threads(nthreads) schedule(static)
	for (int y = 0; y < out_h; y++) {
		const taps_t *t = &taps_y[y];
		uint8_t *d = scaled + (size_t)y * out_w * 4;
		for (int x = 0; x < out_w * 3; x++) {
			int acc = 0;
			for (int k = 0; k < t->n; k++)
				acc += tmp_h[(size_t)t->idx[k] * out_w * 3 + x] * t->w[k];
			d[(x / 3) * 4 + x % 3] = (acc + 32768) >> 16;
		}
	}
}

/* BGRX → I420（BT.709 有限范围） */
// 标量版：处理一对行里 [x0, w) 的像素（NEON 版处理不完的尾巴也用它）
static void i420_rows_scalar(const uint8_t *s0, const uint8_t *s1, uint8_t *y0, uint8_t *y1,
                             uint8_t *u, uint8_t *v, int x0, int w) {
	const int bi = swap_rb ? 2 : 0, ri = swap_rb ? 0 : 2;
	for (int x = x0; x < w; x += 2) {
		int sb = 0, sg = 0, sr = 0;
		const uint8_t *px[4] = {s0 + x * 4, s0 + x * 4 + 4, s1 + x * 4, s1 + x * 4 + 4};
		uint8_t *py[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
		for (int k = 0; k < 4; k++) {
			int b = px[k][bi], g = px[k][1], r = px[k][ri];
			*py[k] = ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
			sb += b; sg += g; sr += r;
		}
		u[x / 2] = ((-26 * sr - 87 * sg + 113 * sb + 512) >> 10) + 128;
		v[x / 2] = ((112 * sr - 102 * sg - 10 * sb + 512) >> 10) + 128;
	}
}

#ifdef __aarch64__
static inline uint8x16_t luma16(uint8x16_t r, uint8x16_t g, uint8x16_t b) {
	// Y = ((47R + 157G + 16B + 128) >> 8) + 16，中间值最大 56228，u16 放得下
	uint16x8_t lo = vmull_u8(vget_low_u8(r), vdup_n_u8(47));
	lo = vmlal_u8(lo, vget_low_u8(g), vdup_n_u8(157));
	lo = vmlal_u8(lo, vget_low_u8(b), vdup_n_u8(16));
	uint16x8_t hi = vmull_high_u8(r, vdupq_n_u8(47));
	hi = vmlal_high_u8(hi, g, vdupq_n_u8(157));
	hi = vmlal_high_u8(hi, b, vdupq_n_u8(16));
	return vaddq_u8(vcombine_u8(vrshrn_n_u16(lo, 8), vrshrn_n_u16(hi, 8)), vdupq_n_u8(16));
}

// 一次处理两行各 16 个像素
static int i420_rows_neon(const uint8_t *s0, const uint8_t *s1, uint8_t *y0, uint8_t *y1,
                          uint8_t *u, uint8_t *v, int w) {
	const int bi = swap_rb ? 2 : 0, ri = swap_rb ? 0 : 2;
	int x = 0;
	for (; x + 16 <= w; x += 16) {
		uint8x16x4_t a = vld4q_u8(s0 + x * 4), c = vld4q_u8(s1 + x * 4);
		vst1q_u8(y0 + x, luma16(a.val[ri], a.val[1], a.val[bi]));
		vst1q_u8(y1 + x, luma16(c.val[ri], c.val[1], c.val[bi]));
		// 2×2 求平均（≤255），再按有符号 16 位算色度，范围在 ±28815 内不会溢出
		int16x8_t r = vreinterpretq_s16_u16(vrshrq_n_u16(vpadalq_u8(vpaddlq_u8(a.val[ri]), c.val[ri]), 2));
		int16x8_t g = vreinterpretq_s16_u16(vrshrq_n_u16(vpadalq_u8(vpaddlq_u8(a.val[1]), c.val[1]), 2));
		int16x8_t b = vreinterpretq_s16_u16(vrshrq_n_u16(vpadalq_u8(vpaddlq_u8(a.val[bi]), c.val[bi]), 2));
		int16x8_t cu = vmlsq_n_s16(vmlsq_n_s16(vmulq_n_s16(b, 113), g, 87), r, 26);
		int16x8_t cv = vmlsq_n_s16(vmlsq_n_s16(vmulq_n_s16(r, 112), g, 102), b, 10);
		vst1_u8(u + x / 2, vqmovun_s16(vaddq_s16(vrshrq_n_s16(cu, 8), vdupq_n_s16(128))));
		vst1_u8(v + x / 2, vqmovun_s16(vaddq_s16(vrshrq_n_s16(cv, 8), vdupq_n_s16(128))));
	}
	return x;
}
#endif

static void bgrx_to_i420(x264_picture_t *pic, const uint8_t *src, int w, int h) {
	uint8_t *Y = pic->img.plane[0], *U = pic->img.plane[1], *V = pic->img.plane[2];
	int ys = pic->img.i_stride[0], us = pic->img.i_stride[1], vs = pic->img.i_stride[2];
	#pragma omp parallel for num_threads(nthreads) schedule(static)
	for (int y = 0; y < h; y += 2) {
		const uint8_t *s0 = src + (size_t)y * w * 4, *s1 = s0 + (size_t)w * 4;
		uint8_t *y0 = Y + (size_t)y * ys, *y1 = y0 + ys;
		uint8_t *u = U + (size_t)(y / 2) * us, *v = V + (size_t)(y / 2) * vs;
		int x0 = 0;
#ifdef __aarch64__
		x0 = i420_rows_neon(s0, s1, y0, y1, u, v, w);
#endif
		i420_rows_scalar(s0, s1, y0, y1, u, v, x0, w);
	}
}

/* 编码线程 */
static void *enc_main(void *arg) {
	int64_t pts = 0;
	pthread_mutex_lock(&enc_mu);
	for (;;) {
		// 有新图就编新图；画面静止时收到关键帧请求，就把最近一张重编成 IDR
		while (enc_pending < 0 && !(idr_next && last_filled >= 0))
			pthread_cond_wait(&enc_cv, &enc_mu);
		int idx = enc_pending >= 0 ? enc_pending : last_filled;
		enc_pending = -1;
		enc_using = idx;
		bool idr = idr_next;
		idr_next = false;
		pthread_cond_broadcast(&enc_cv);
		pthread_mutex_unlock(&enc_mu);

		double t0 = now_ms();
		x264_picture_t *pic = &pics[idx], out;
		x264_nal_t *nals;
		int nnal;
		pic->i_pts = pts++;
		pic->i_type = idr ? X264_TYPE_IDR : X264_TYPE_AUTO;
		int n = x264_encoder_encode(enc, &nals, &nnal, pic, &out);
		if (n < 0) die("x264_encoder_encode 失败");
		if (n > 0) send_pkt(2, out.b_keyframe ? 1 : 0, nals[0].p_payload, n); // 同一次调用的 NAL 在内存里连续
		double t1 = now_ms();

		pthread_mutex_lock(&enc_mu);
		enc_using = -1;
		st_enc_sum += t1 - t0;
		st_enc_n++;
		pthread_cond_broadcast(&enc_cv);
	}
	return NULL;
}

// 主线程：拿一张编码线程没在用的图来写
static int acquire_pic(void) {
	pthread_mutex_lock(&enc_mu);
	int idx = last_filled < 0 ? 0 : 1 - last_filled;
	while (enc_using == idx || enc_pending == idx)
		pthread_cond_wait(&enc_cv, &enc_mu);
	pthread_mutex_unlock(&enc_mu);
	return idx;
}

static void submit_pic(int idx) {
	pthread_mutex_lock(&enc_mu);
	while (enc_pending >= 0) // 上一张还没被取走就等，保证顺序，也是对编码速度的背压
		pthread_cond_wait(&enc_cv, &enc_mu);
	enc_pending = idx;
	last_filled = idx;
	pthread_cond_broadcast(&enc_cv);
	pthread_mutex_unlock(&enc_mu);
}

static void request_idr(void) {
	pthread_mutex_lock(&enc_mu);
	idr_next = true;
	pthread_cond_broadcast(&enc_cv);
	pthread_mutex_unlock(&enc_mu);
}

static void setup_encoder(void) {
	x264_param_t p;
	if (x264_param_default_preset(&p, "ultrafast", "zerolatency") < 0) die("x264 preset 失败");
	p.i_width = out_w;
	p.i_height = out_h;
	p.i_csp = X264_CSP_I420;
	p.i_fps_num = max_fps;
	p.i_fps_den = 1;
	p.i_threads = nthreads;
	p.i_keyint_max = X264_KEYINT_MAX_INFINITE; // 走 TCP 不丢包；新连接重启进程，解码出错时浏览器发 'k'
	p.b_annexb = 1;
	p.b_repeat_headers = 1;
	p.rc.i_rc_method = X264_RC_CRF;
	p.rc.f_rf_constant = crf;
	p.vui.i_colorprim = 1;  // BT.709
	p.vui.i_transfer = 1;
	p.vui.i_colmatrix = 1;
	p.vui.b_fullrange = 0;
	p.i_log_level = X264_LOG_WARNING;
	enc = x264_encoder_open(&p);
	if (!enc) die("x264_encoder_open 失败");
	for (int i = 0; i < 2; i++)
		if (x264_picture_alloc(&pics[i], X264_CSP_I420, out_w, out_h) < 0) die("x264_picture_alloc 失败");

	// 从 SPS 取 profile/constraint/level，拼 WebCodecs 的 codec 串
	x264_nal_t *nals;
	int nnal;
	char codec[32] = "avc1.42E028";
	x264_encoder_headers(enc, &nals, &nnal);
	for (int i = 0; i < nnal; i++) {
		if (nals[i].i_type != NAL_SPS) continue;
		const uint8_t *s = nals[i].p_payload;
		int k = 0;
		while (k < nals[i].i_payload && s[k] == 0) k++; // 跳过起始码
		k++;                                             // 0x01
		if (k + 3 < nals[i].i_payload)
			snprintf(codec, sizeof codec, "avc1.%02X%02X%02X", s[k + 1], s[k + 2], s[k + 3]);
	}
	char js[256];
	int n = snprintf(js, sizeof js,
	                 "{\"type\":\"config\",\"codec\":\"%s\",\"w\":%d,\"h\":%d,\"srcw\":%d,\"srch\":%d}",
	                 codec, out_w, out_h, src_w, src_h);
	send_pkt(1, 0, js, n);
	LOG("编码 %dx%d（源 %dx%d）codec=%s 最大 %d 帧/秒 crf=%d 线程=%d\n",
	    out_w, out_h, src_w, src_h, codec, max_fps, crf, nthreads);

	pthread_t th;
	if (pthread_create(&th, NULL, enc_main, NULL)) die("建编码线程失败");
	pthread_detach(th);
}

/* 采集缓冲区 */
static void setup_buffer(void) {
	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) die("打不开 /dev/dri/card0（用户要在 video 组）");
	drm_magic_t magic;
	if (!mbs) die("treeland 没有 mali_buffer_sharing（libtl-malibuf 没加载？）");
	if (drmGetMagic(drm_fd, &magic)) die("drmGetMagic 失败");
	wl_proxy_marshal(mbs, 1, magic);
	wl_display_roundtrip(dpy);

	struct drm_mode_create_dumb c = {.width = src_w, .height = src_h, .bpp = 32};
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) die("CREATE_DUMB 失败");
	if (drmPrimeHandleToFD(drm_fd, c.handle, DRM_CLOEXEC | DRM_RDWR, &prime_fd))
		die("PRIME 导出失败（DRM 认证没通过？）");
	struct drm_mode_map_dumb m = {.handle = c.handle};
	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) die("MAP_DUMB 失败");
	map_size = c.size;
	map_stride = c.pitch;
	map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, drm_fd, m.offset);
	if (map == MAP_FAILED) die("mmap dumb buffer 失败");

	struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dmabuf);
	uint64_t mod = DRM_FORMAT_MOD_INVALID;
	zwp_linux_buffer_params_v1_add(p, prime_fd, 0, 0, map_stride, mod >> 32, mod & 0xffffffff);
	wlbuf = zwp_linux_buffer_params_v1_create_immed(p, src_w, src_h, fs.dma_fmt, 0);
	zwp_linux_buffer_params_v1_destroy(p);

	if (!force_cpu)
		use_gpu = gpu_init(drm_fd, prime_fd, fs.dma_fmt, src_w, src_h, map_stride, out_w, out_h) == 0;
	if (use_gpu) return;
	frame = malloc((size_t)src_w * src_h * 4);
	if (out_w != src_w || out_h != src_h) {
		taps_x = make_taps(src_w, out_w);
		taps_y = make_taps(src_h, out_h);
		tmp_h = malloc((size_t)src_h * out_w * 3 * sizeof *tmp_h);
		scaled = malloc((size_t)out_w * out_h * 4);
	}
}

/* 每帧处理 */
static double st_t0, st_copy, st_read, st_wait, st_conv;
static int st_n;

// 从 dumb buffer 读回普通内存。写合并内存只有大块顺序拷贝快，单线程 memcpy 比多线程分行拷还快
static void readout(void) {
	struct dma_buf_sync s = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
	ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &s);
	if (map_stride == (uint32_t)src_w * 4) {
		memcpy(frame, map, (size_t)map_stride * src_h);
	} else {
		for (int y = 0; y < src_h; y++)
			memcpy(frame + (size_t)y * src_w * 4, map + (size_t)y * map_stride, (size_t)src_w * 4);
	}
	s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &s);
}

// t1→t2 是 readout()（CPU 路径）或 gpu_convert()（GPU 路径）
static void convert_encode(double t_req, double t1, double t2) {
	int idx = acquire_pic();
	double t3 = now_ms();
	x264_image_t *im = &pics[idx].img;
	if (use_gpu) {
		gpu_readout(im->plane[0], im->i_stride[0], im->plane[1], im->i_stride[1], im->plane[2], im->i_stride[2]);
	} else if (scaled) {
		scale_bgrx();
		bgrx_to_i420(&pics[idx], scaled, out_w, out_h);
	} else {
		bgrx_to_i420(&pics[idx], frame, src_w, src_h);
	}
	double t4 = now_ms();
	submit_pic(idx);

	st_copy += t1 - t_req; st_read += t2 - t1; st_wait += t3 - t2; st_conv += t4 - t3; st_n++;
	if (t4 - st_t0 >= 10000) {
		pthread_mutex_lock(&enc_mu);
		double enc_avg = st_enc_n ? st_enc_sum / st_enc_n : 0;
		st_enc_sum = 0; st_enc_n = 0;
		pthread_mutex_unlock(&enc_mu);
		const char *fmt = use_gpu
			? "rv-cap: %.1f 帧/秒；每帧 请求→就绪 %.1f / GPU 转换 %.1f / 等编码线程 %.1f / 读出 I420 %.1f / 编码 %.1f ms\n"
			: "rv-cap: %.1f 帧/秒；每帧 请求→就绪 %.1f / 读出 %.1f / 等编码线程 %.1f / 缩放转换 %.1f / 编码 %.1f ms\n";
		fprintf(stderr, fmt, st_n * 1000.0 / (t4 - st_t0), st_copy / st_n, st_read / st_n, st_wait / st_n,
		        st_conv / st_n, enc_avg);
		st_t0 = t4; st_copy = st_read = st_wait = st_conv = 0; st_n = 0;
	}
}

/* stdin 命令 */
static void handle_stdin(void) {
	char buf[64];
	ssize_t n = read(0, buf, sizeof buf);
	if (n <= 0) exit(0);
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == 'k') {
			request_idr(); // 画面静止时编码线程会把最近一张重编成 IDR
		} else if (buf[i] == 'q') {
			exit(0);
		}
	}
}

int main(int argc, char **argv) {
	int opt;
	while ((opt = getopt(argc, argv, "w:h:r:q:t:nC")) != -1) {
		switch (opt) {
		case 'w': out_w = atoi(optarg); break;
		case 'h': out_h = atoi(optarg); break;
		case 'r': max_fps = atoi(optarg); break;
		case 'q': crf = atoi(optarg); break;
		case 't': nthreads = atoi(optarg); break;
		case 'n': overlay_cursor = false; break;
		case 'C': force_cpu = true; break;
		default:
			fprintf(stderr, "用法: %s [-w 宽] [-h 高] [-r 最大帧率] [-q crf] [-t 线程] [-n] [-C]\n", argv[0]);
			return 1;
		}
	}
	if (max_fps < 1) max_fps = 1;
	if (nthreads < 1) nthreads = 1;
	signal(SIGPIPE, SIG_IGN);

	dpy = wl_display_connect(NULL);
	if (!dpy) die("连不上 Wayland（WAYLAND_DISPLAY/XDG_RUNTIME_DIR 对吗？）");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!output || !scm || !dmabuf) die("合成器缺 wl_output / wlr-screencopy / linux-dmabuf");

	// 第一帧：协商尺寸、建缓冲区和编码器，用不带 damage 的 copy 立刻拿到画面
	// 如果刚亮屏，合成器可能还没把 dmabuf 能力加上，最多重试几次，别让页面反复重连。
	struct zwlr_screencopy_frame_v1 *fr;
	int retry;
	for (retry = 0; retry < 30; retry++) {
		fr = zwlr_screencopy_manager_v1_capture_output(scm, overlay_cursor, output);
		zwlr_screencopy_frame_v1_add_listener(fr, &f_l, NULL);
		while (!fs.done && !fs.failed)
			if (wl_display_dispatch(dpy) < 0) die("Wayland 连接断开");
		if (fs.failed || !fs.has_dma) {
			const char *msg = "合成器暂时不能采集（屏幕可能在休眠），等亮屏 ...\n";
			if (retry == 0) send_pkt(3, 1, msg, strlen(msg));
			LOG("%s", msg);
			zwlr_screencopy_frame_v1_destroy(fr);
			memset(&fs, 0, sizeof fs);
			poll(NULL, 0, 1000);
			wl_display_dispatch_pending(dpy);
			continue;
		}
		break;
	}
	if (retry >= 30) {
		const char *msg = "屏幕一直不能采集，唤醒屏幕后会自动恢复。\n";
		send_pkt(3, 0, msg, strlen(msg));
		die(msg);
	}
	src_w = fs.dma_w;
	src_h = fs.dma_h;
	if (fs.dma_fmt == DRM_FORMAT_XBGR8888 || fs.dma_fmt == DRM_FORMAT_ABGR8888)
		swap_rb = true;
	else if (fs.dma_fmt != DRM_FORMAT_XRGB8888 && fs.dma_fmt != DRM_FORMAT_ARGB8888)
		die("不认识的像素格式（只支持 XRGB/ARGB/XBGR/ABGR 8888）");
	if (!out_w && !out_h) { out_w = src_w; out_h = src_h; }
	else if (!out_h) out_h = (int)lround((double)out_w * src_h / src_w);
	else if (!out_w) out_w = (int)lround((double)out_h * src_w / src_h);
	out_w &= force_cpu ? ~1 : ~7; // GPU 转换一个像素装 4 个字节、色度再减半，宽要是 8 的倍数
	out_h &= ~1;
	if (out_w > src_w || out_h > src_h || out_w * 3 < src_w || out_h * 3 < src_h)
		die("输出尺寸只能在源尺寸的 1/3 到 1 倍之间");
	setup_buffer();
	setup_encoder();
	st_t0 = now_ms();

	double t_req = now_ms();
	zwlr_screencopy_frame_v1_copy(fr, wlbuf);
	bool copying = true, first_frame = true;
	double next_due = 0;
	double interval = 1000.0 / max_fps;

	struct pollfd pfd[2] = {{.fd = wl_display_get_fd(dpy), .events = POLLIN}, {.fd = 0, .events = POLLIN}};
	for (;;) {
		wl_display_dispatch_pending(dpy);
		if (copying && fs.failed) die("采集失败（输出变化？）");
		if (copying && fs.done && !fs.has_dma) die("合成器没有提供 dmabuf 采集");
		if (copying && fs.done && (fs.dma_w != (uint32_t)src_w || fs.dma_h != (uint32_t)src_h))
			die("输出尺寸变了，退出由上层重启");
		if (copying && fs.ready) {
			if (!use_gpu && first_frame) {
				// treeland 发 ready 时它的 GPU 拷贝可能还没写完，Mali 也没挂 dma-buf fence，DMA_BUF_IOCTL_SYNC
				// 等不到；CPU 从上往下读会先读到缓冲区里的旧内容。之后的帧旧内容就是上一帧，看不出来，
				// 第一帧却是缓冲区里原有的别的画面（顶部一条错位的杂色带），丢掉。GPU 路径在 GPU 上排好了序，没这问题。
				first_frame = false;
				zwlr_screencopy_frame_v1_destroy(fr);
				copying = false;
				continue;
			}
			first_frame = false;
			double t1 = now_ms();
			if (use_gpu)
				gpu_convert(); // 返回时 GPU 已读完源缓冲区
			else
				readout();
			double t2 = now_ms();
			zwlr_screencopy_frame_v1_destroy(fr);
			copying = false;
			next_due = t_req + interval;
			bool start_now = now_ms() >= next_due;
			if (start_now) {
				// 帧已读出，缓冲区可以复用：马上发下一帧请求，让合成器的 GPU 拷贝和下面的转换、编码并行。
				// wlroots 在 capture_output 时就已确定 buffer 参数，不等 buffer_done 直接 copy 没问题；
				// 尺寸若变了，后面收到 buffer_done 时会发现并退出。
				memset(&fs, 0, sizeof fs);
				fr = zwlr_screencopy_manager_v1_capture_output(scm, overlay_cursor, output);
				zwlr_screencopy_frame_v1_add_listener(fr, &f_l, NULL);
				zwlr_screencopy_frame_v1_copy_with_damage(fr, wlbuf);
				wl_display_flush(dpy);
				copying = true;
			}
			double t_prev = t_req;
			if (start_now) t_req = now_ms();
			convert_encode(t_prev, t1, t2);
			continue;
		}
		if (!copying && now_ms() >= next_due) {
			memset(&fs, 0, sizeof fs);
			fr = zwlr_screencopy_manager_v1_capture_output(scm, overlay_cursor, output);
			zwlr_screencopy_frame_v1_add_listener(fr, &f_l, NULL);
			zwlr_screencopy_frame_v1_copy_with_damage(fr, wlbuf);
			copying = true;
			t_req = now_ms();
		}

		if (wl_display_prepare_read(dpy) != 0) continue;
		wl_display_flush(dpy);
		int timeout = -1;
		if (!copying) {
			timeout = (int)ceil(next_due - now_ms());
			if (timeout < 0) timeout = 0;
		}
		if (poll(pfd, 2, timeout) < 0 && errno != EINTR) {
			wl_display_cancel_read(dpy);
			die("poll 失败");
		}
		if (pfd[0].revents & POLLIN) {
			if (wl_display_read_events(dpy) < 0) die("Wayland 连接断开");
		} else {
			wl_display_cancel_read(dpy);
		}
		if (pfd[0].revents & (POLLERR | POLLHUP)) die("Wayland 连接断开");
		if (pfd[1].revents) handle_stdin();
	}
}
