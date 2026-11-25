/*
 * treeland 的 LD_PRELOAD 垫片（华为擎云 L540 / hisi-drm，没有可用的 GPU 用户态驱动，全程 llvmpipe）。
 * 1) hisi-drm 的 renderD128 只是显示控制器，Mesa 没有对应驱动；wlroots 默认在 render 节点上建 GBM，
 *    回退到 kms_swrast 后要建 dumb buffer，而内核不允许在 render 节点上建 dumb buffer（EACCES），swapchain 分配全部失败。
 *    让 libdrm 报告"没有 render 节点"，wlroots 的 reopen_drm_node() 就会改用 primary 节点 card0（并用 DRM master 认证）。
 * 2) kms_swrast 导出 dumb buffer 的 dmabuf 时不带 DRM_RDWR，llvmpipe 以读写方式 mmap 导入时 EACCES
 *    （"dmabuf import failed to mmap"），treeland 随后段错误。导出时补上 DRM_RDWR。
 * 编译：gcc -O2 -shared -fPIC -o libtl-norender.so tl-norender.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>

char *drmGetRenderDeviceNameFromFd(int fd)
{
    (void) fd;
    return NULL;
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    static int (*real)(int, uint32_t, uint32_t, int *);
    int r;

    if (!real)
        real = (int (*)(int, uint32_t, uint32_t, int *)) dlsym(RTLD_NEXT, "drmPrimeHandleToFD");
    r = real(fd, handle, flags | O_RDWR, prime_fd); /* DRM_RDWR == O_RDWR */
    if (r != 0)
        r = real(fd, handle, flags, prime_fd);
    return r;
}
