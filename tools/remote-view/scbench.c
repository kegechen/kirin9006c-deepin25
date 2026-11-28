// scbench: 对比 treeland 上 wlr-screencopy 的 shm 与 dmabuf 两条路径
// 用法: scbench <shm|dumb> [帧数] [保存最后一帧的 ppm 路径]
//   shm : 合成器把画面读回到客户端的 shm 内存（grim/wf-recorder 走的路）
//   dumb: 客户端在 card0 上建线性 dumb buffer，以 dmabuf 交给合成器让 GPU 拷进来，
//         再由客户端 CPU 从 dumb buffer 读出
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#include <wayland-client.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

static struct wl_output *output;
static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *scm;
static struct zwp_linux_dmabuf_v1 *dmabuf;

static struct {
	uint32_t shm_fmt, shm_w, shm_h, shm_stride;
	uint32_t dma_fmt, dma_w, dma_h;
	bool has_shm, has_dma, done, ready, failed;
	uint32_t flags;
} fs;

static double now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// mali_buffer_sharing（treeland 由 libtl-malibuf 提供），这里只用它的 auth 请求让 card0 fd 通过 DRM 认证
static const struct wl_interface *mbs_types[] = {&wl_buffer_interface, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
static const struct wl_message mbs_requests[] = {{"create_buffer", "niiuuuuh", mbs_types}, {"auth", "u", mbs_types + 1}};
static const struct wl_message mbs_events[] = {{"alloc_device", "s", mbs_types + 1}};
static const struct wl_interface mbs_interface = {"mali_buffer_sharing", 5, 2, mbs_requests, 1, mbs_events};
static struct wl_proxy *mbs;

static struct { uint32_t fmt; uint64_t mod; } mods[512];
static int n_mods;
static void dm_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f) {}
static void dm_modifier(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f, uint32_t hi, uint32_t lo) {
	if (n_mods < 512) { mods[n_mods].fmt = f; mods[n_mods].mod = ((uint64_t)hi << 32) | lo; n_mods++; }
}
static const struct zwp_linux_dmabuf_v1_listener dm_l = {dm_format, dm_modifier};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		scm = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, ver < 3 ? ver : 3);
	else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
		dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(dmabuf, &dm_l, NULL);
	} else if (!strcmp(iface, "mali_buffer_sharing"))
		mbs = wl_registry_bind(r, name, &mbs_interface, ver < 5 ? ver : 5);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

static void f_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride) {
	fs.has_shm = true; fs.shm_fmt = fmt; fs.shm_w = w; fs.shm_h = h; fs.shm_stride = stride;
}
static void f_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fl) { fs.flags = fl; }
static void f_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c) { fs.ready = true; }
static void f_failed(void *d, struct zwlr_screencopy_frame_v1 *f) { fs.failed = true; }
static void f_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {}
static void f_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h) {
	fs.has_dma = true; fs.dma_fmt = fmt; fs.dma_w = w; fs.dma_h = h;
}
static void f_done(void *d, struct zwlr_screencopy_frame_v1 *f) { fs.done = true; }
static const struct zwlr_screencopy_frame_v1_listener f_l = {
	f_buffer, f_flags, f_ready, f_failed, f_damage, f_dmabuf, f_done};

static void save_ppm(const char *path, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, uint32_t fourcc) {
	FILE *fp = fopen(path, "wb");
	if (!fp) { perror(path); return; }
	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	bool bgr = (fourcc == DRM_FORMAT_XRGB8888 || fourcc == DRM_FORMAT_ARGB8888);
	uint8_t *row = malloc(w * 3);
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *s = px + (size_t)y * stride;
		for (uint32_t x = 0; x < w; x++) {
			row[x * 3 + 0] = bgr ? s[x * 4 + 2] : s[x * 4 + 0];
			row[x * 3 + 1] = s[x * 4 + 1];
			row[x * 3 + 2] = bgr ? s[x * 4 + 0] : s[x * 4 + 2];
		}
		fwrite(row, 1, w * 3, fp);
	}
	free(row);
	fclose(fp);
	fprintf(stderr, "已保存 %s\n", path);
}

int main(int argc, char **argv) {
	const char *mode = argc > 1 ? argv[1] : "dumb";
	int frames = argc > 2 ? atoi(argv[2]) : 20;
	const char *ppm = argc > 3 ? argv[3] : NULL;
	bool use_dumb = !strcmp(mode, "dumb");

	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "连不上 Wayland\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!output || !scm || !shm || (use_dumb && !dmabuf)) { fprintf(stderr, "缺协议\n"); return 1; }
	// SCB_MOD=linear 用 LINEAR modifier；默认 invalid（隐式 modifier，由驱动按线性处理）
	const char *modenv = getenv("SCB_MOD");
	uint64_t buf_mod = (modenv && !strcmp(modenv, "linear")) ? DRM_FORMAT_MOD_LINEAR : DRM_FORMAT_MOD_INVALID;

	struct wl_buffer *buf = NULL;
	uint8_t *map = NULL, *dst = NULL;
	size_t size = 0;
	uint32_t stride = 0, w = 0, h = 0, fourcc = 0;
	int drm_fd = -1, prime_fd = -1;
	double sum_copy = 0, sum_read = 0, max_copy = 0;
	int counted = 0;

	for (int i = 0; i < frames; i++) {
		memset(&fs, 0, sizeof fs);
		struct zwlr_screencopy_frame_v1 *fr = zwlr_screencopy_manager_v1_capture_output(scm, 0, output);
		zwlr_screencopy_frame_v1_add_listener(fr, &f_l, NULL);
		while (!fs.done && !fs.failed)
			if (wl_display_dispatch(dpy) < 0) { fprintf(stderr, "dispatch 失败\n"); return 1; }
		if (fs.failed) { fprintf(stderr, "第 %d 帧 buffer 协商失败\n", i); return 1; }

		if (!buf) {
			fprintf(stderr, "shm 提议: fmt=0x%x %ux%u stride=%u\n", fs.shm_fmt, fs.shm_w, fs.shm_h, fs.shm_stride);
			if (fs.has_dma)
				fprintf(stderr, "dmabuf 提议: %.4s %ux%u\n", (char *)&fs.dma_fmt, fs.dma_w, fs.dma_h);
			else
				fprintf(stderr, "合成器没有提议 dmabuf\n");

			if (use_dumb) {
				if (!fs.has_dma) return 1;
				w = fs.dma_w; h = fs.dma_h; fourcc = fs.dma_fmt;
				for (int k = 0; k < n_mods; k++)
					if (mods[k].fmt == fourcc)
						fprintf(stderr, "  合成器可导入 %.4s modifier 0x%016llx\n", (char *)&fourcc,
						        (unsigned long long)mods[k].mod);
				drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
				if (drm_fd < 0) { perror("open card0"); return 1; }
				drm_magic_t magic;
				if (mbs && !drmGetMagic(drm_fd, &magic)) {
					wl_proxy_marshal(mbs, 1, magic);
					wl_display_roundtrip(dpy);
					fprintf(stderr, "已通过 mali_buffer_sharing 发 auth(%u)\n", magic);
				}
				struct drm_mode_create_dumb c = {.width = w, .height = h, .bpp = 32};
				if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { perror("CREATE_DUMB"); return 1; }
				stride = c.pitch; size = c.size;
				if (drmPrimeHandleToFD(drm_fd, c.handle, DRM_CLOEXEC | DRM_RDWR, &prime_fd)) { perror("PrimeHandleToFD"); return 1; }
				struct drm_mode_map_dumb m = {.handle = c.handle};
				if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { perror("MAP_DUMB"); return 1; }
				map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, m.offset);
				if (map == MAP_FAILED) { perror("mmap dumb"); return 1; }
				fprintf(stderr, "dumb buffer: pitch=%u size=%zu\n", stride, size);
				struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dmabuf);
				zwp_linux_buffer_params_v1_add(p, prime_fd, 0, 0, stride, buf_mod >> 32, buf_mod & 0xffffffff);
				fprintf(stderr, "用 modifier 0x%016llx 建 wl_buffer\n", (unsigned long long)buf_mod);
				buf = zwp_linux_buffer_params_v1_create_immed(p, w, h, fourcc, 0);
				zwp_linux_buffer_params_v1_destroy(p);
			} else {
				w = fs.shm_w; h = fs.shm_h; stride = fs.shm_stride; size = (size_t)stride * h;
				fourcc = fs.shm_fmt == WL_SHM_FORMAT_XRGB8888 ? DRM_FORMAT_XRGB8888
				       : fs.shm_fmt == WL_SHM_FORMAT_ARGB8888 ? DRM_FORMAT_ARGB8888 : fs.shm_fmt;
				int fd = memfd_create("scbench", MFD_CLOEXEC);
				ftruncate(fd, size);
				map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
				struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
				buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, fs.shm_fmt);
				wl_shm_pool_destroy(pool);
				close(fd);
			}
			dst = malloc(size);
		}

		double t0 = now_ms();
		zwlr_screencopy_frame_v1_copy(fr, buf);
		while (!fs.ready && !fs.failed)
			if (wl_display_dispatch(dpy) < 0) { fprintf(stderr, "dispatch 失败（多半是协议错误）\n"); return 1; }
		double t1 = now_ms();
		if (fs.failed) { fprintf(stderr, "第 %d 帧 copy 失败\n", i); return 1; }

		if (prime_fd >= 0) {
			struct dma_buf_sync s = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
			ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &s);
		}
		memcpy(dst, map, size);
		if (prime_fd >= 0) {
			struct dma_buf_sync s = {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
			ioctl(prime_fd, DMA_BUF_IOCTL_SYNC, &s);
		}
		double t2 = now_ms();
		zwlr_screencopy_frame_v1_destroy(fr);

		if (i > 0) { // 第一帧含建 buffer、导入等一次性开销，不计入
			sum_copy += t1 - t0; sum_read += t2 - t1; counted++;
			if (t1 - t0 > max_copy) max_copy = t1 - t0;
		}
		fprintf(stderr, "帧 %2d: 合成器拷贝 %.1f ms, CPU 读出 %.1f ms\n", i, t1 - t0, t2 - t1);
	}

	if (counted) {
		double per = (sum_copy + sum_read) / counted;
		printf("[%s] %ux%u, %zu 字节/帧, 平均 合成器拷贝 %.1f ms(最大 %.1f) + CPU 读出 %.1f ms = %.1f ms/帧 ≈ %.1f 帧/秒, CPU 读出带宽 %.0f MB/s\n",
		       mode, w, h, size, sum_copy / counted, max_copy, sum_read / counted, per, 1000.0 / per,
		       size / 1e6 / (sum_read / counted / 1000.0));
	}
	if (ppm) save_ppm(ppm, dst, w, h, stride, fourcc);
	return 0;
}
