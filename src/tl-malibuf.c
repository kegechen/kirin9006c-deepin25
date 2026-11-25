/*
 * tl-malibuf：给 treeland 补上厂商 libmali 客户端需要的私有协议 mali_buffer_sharing v5。
 *
 * 没有这个 global 时，libmali 作为 Wayland 客户端只枚举一遍 registry 就让 eglInitialize 返回
 * EGL_NOT_INITIALIZED（0x3001），所以 dde-shell、控制中心这些客户端只能走 Mesa/llvmpipe。
 *
 * 协议（与 r23p0 libmali 内嵌的 wl_interface 逐字一致，参考 panfork 的公开逆向实现）：
 *   event   alloc_device(name: string)          bind 后立刻发，客户端据此 drmOpen(NULL, name)
 *   request create_buffer(id: new_id wl_buffer, width, height: int, stride, format, unk1, unk2: uint, fd)
 *           fd 是客户端自己分配的单平面 dma-buf（海思上由 DRM_IOCTL_HISI_GEM_CREATE 分配，需先 auth），
 *           format 是 DRM fourcc，unk1/unk2 是 modifier 的高/低 32 位（默认 ARM AFBC）
 *   request auth(magic: uint)                   转给 drmAuthMagic（用 DRM master fd），无回复事件
 *
 * 做法：LD_PRELOAD 进 treeland，不改 treeland 源码。
 *   - 挂 wlr_allocator_autocreate(backend, renderer)：记下 wlr_backend_get_drm_fd(backend)（DRM master）。
 *   - 挂 wlr_drm_create(display, renderer)：调原函数后再注册 mali_buffer_sharing global，
 *     并向 wlroots 注册 wl_buffer 资源接口；create_buffer 照抄 wlroots types/wlr_drm.c 的 prime buffer，
 *     包成带 get_dmabuf 的 wlr_buffer，后面的纹理导入走 wlroots 现成的 dmabuf 路径。
 *   - 所有 wlr_* 函数都在运行时 dlsym，不产生对 libwaylib-wlroots 的 NEEDED 依赖。
 *
 * 环境变量：TL_MALIBUF_DISABLE=1 关闭；TL_MALIBUF_DEVICE=<串> 覆盖 alloc_device 发的名字。
 * 结构体布局对照 treeland 0.11.0 捆绑的 wlroots 0.20.2（libwaylib-wlroots.so.0.20），升级 wlroots 后需复核。
 *
 * 编译：gcc -O2 -Wall -shared -fPIC -o libtl-malibuf.so tl-malibuf.c $(pkg-config --cflags --libs libdrm wayland-server) -ldl
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

/* ---- wlroots 0.20.2 的结构体（只取用到的部分，布局必须一致） ---- */
struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_drm;
struct wlr_shm_attributes;

#define WLR_DMABUF_MAX_PLANES 4
struct wlr_dmabuf_attributes {
    int32_t  width, height;
    uint32_t format;
    uint64_t modifier;
    int      n_planes;
    uint32_t offset[WLR_DMABUF_MAX_PLANES];
    uint32_t stride[WLR_DMABUF_MAX_PLANES];
    int      fd[WLR_DMABUF_MAX_PLANES];
};

struct wlr_addon_set {
    struct wl_list addons;
};

struct wlr_buffer {
    const struct wlr_buffer_impl *impl;
    int    width, height;
    bool   dropped;
    size_t n_locks;
    bool   accessing_data_ptr;
    struct {
        struct wl_signal destroy;
        struct wl_signal release;
    } events;
    struct wlr_addon_set addons;
};

struct wlr_buffer_impl {
    void (*destroy)(struct wlr_buffer *buffer);
    bool (*get_dmabuf)(struct wlr_buffer *buffer, struct wlr_dmabuf_attributes *attribs);
    bool (*get_shm)(struct wlr_buffer *buffer, struct wlr_shm_attributes *attribs);
    bool (*begin_data_ptr_access)(struct wlr_buffer *buffer, uint32_t flags, void **data,
                                  uint32_t *format, size_t *stride);
    void (*end_data_ptr_access)(struct wlr_buffer *buffer);
};

struct wlr_buffer_resource_interface {
    const char *name;
    bool (*is_instance)(struct wl_resource *resource);
    struct wlr_buffer *(*from_resource)(struct wl_resource *resource);
};

/* ---- 运行时解析的 wlroots 函数 ---- */
static struct {
    void (*buffer_init)(struct wlr_buffer *, const struct wlr_buffer_impl *, int, int);
    void (*buffer_finish)(struct wlr_buffer *);
    void (*buffer_drop)(struct wlr_buffer *);
    void (*register_resource_interface)(const struct wlr_buffer_resource_interface *);
    void (*dmabuf_finish)(struct wlr_dmabuf_attributes *);
    int (*backend_get_drm_fd)(struct wlr_backend *);
    int (*renderer_get_drm_fd)(struct wlr_renderer *);
} W;

static int  g_master_fd = -1;
static int  g_logged_buffers;
static bool g_global_created;

#define LOG(...) fprintf(stderr, "tl-malibuf: " __VA_ARGS__)

static bool resolve_wlr(void)
{
#define R(field, name)                                   \
    do {                                                 \
        *(void **)&W.field = dlsym(RTLD_DEFAULT, name);  \
        if (!W.field) {                                  \
            LOG("缺少符号 %s，不启用\n", name);          \
            return false;                                \
        }                                                \
    } while (0)
    R(buffer_init, "wlr_buffer_init");
    R(buffer_finish, "wlr_buffer_finish");
    R(buffer_drop, "wlr_buffer_drop");
    R(register_resource_interface, "wlr_buffer_register_resource_interface");
    R(dmabuf_finish, "wlr_dmabuf_attributes_finish");
    R(backend_get_drm_fd, "wlr_backend_get_drm_fd");
    R(renderer_get_drm_fd, "wlr_renderer_get_drm_fd");
#undef R
    return true;
}

/* ---- 协议定义（服务端视角） ---- */
static const struct wl_interface *mbs_types[] = {
    &wl_buffer_interface, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};
static const struct wl_message mbs_requests[] = {
    { "create_buffer", "niiuuuuh", mbs_types },
    { "auth", "u", mbs_types + 1 },
};
static const struct wl_message mbs_events[] = {
    { "alloc_device", "s", mbs_types + 1 },
};
static const struct wl_interface mbs_interface = {
    "mali_buffer_sharing", 5, 2, mbs_requests, 1, mbs_events,
};
#define MBS_ALLOC_DEVICE 0

struct mbs_request_impl {
    void (*create_buffer)(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                          int32_t width, int32_t height, uint32_t stride, uint32_t format,
                          uint32_t unk1, uint32_t unk2, int32_t fd);
    void (*auth)(struct wl_client *client, struct wl_resource *resource, uint32_t magic);
};

/* ---- wl_buffer / wlr_buffer 包装（照抄 wlroots types/wlr_drm.c） ---- */
struct mbs_buffer {
    struct wlr_buffer            base;
    struct wl_resource          *resource;
    struct wlr_dmabuf_attributes dmabuf;
    struct wl_listener           release;
};

static void wl_buffer_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = {
    .destroy = wl_buffer_handle_destroy,
};

static const struct wlr_buffer_impl mbs_buffer_impl;

static void mbs_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct mbs_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);

    wl_list_remove(&buffer->release.link);
    W.buffer_finish(wlr_buffer);
    if (buffer->resource)
        wl_resource_set_user_data(buffer->resource, NULL);
    W.dmabuf_finish(&buffer->dmabuf);
    free(buffer);
}

static bool mbs_buffer_get_dmabuf(struct wlr_buffer *wlr_buffer, struct wlr_dmabuf_attributes *out)
{
    struct mbs_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);

    *out = buffer->dmabuf;
    return true;
}

static const struct wlr_buffer_impl mbs_buffer_impl = {
    .destroy    = mbs_buffer_destroy,
    .get_dmabuf = mbs_buffer_get_dmabuf,
};

static bool resource_is_instance(struct wl_resource *resource)
{
    return wl_resource_instance_of(resource, &wl_buffer_interface, &wl_buffer_impl);
}

static struct wlr_buffer *buffer_from_resource(struct wl_resource *resource)
{
    struct mbs_buffer *buffer = wl_resource_get_user_data(resource);

    return buffer ? &buffer->base : NULL;
}

static const struct wlr_buffer_resource_interface mbs_resource_interface = {
    .name          = "mali_buffer_sharing_buffer",
    .is_instance   = resource_is_instance,
    .from_resource = buffer_from_resource,
};

static void wl_buffer_resource_destroyed(struct wl_resource *resource)
{
    struct mbs_buffer *buffer = wl_resource_get_user_data(resource);

    if (!buffer)
        return;
    buffer->resource = NULL;
    W.buffer_drop(&buffer->base);
}

static void mbs_buffer_handle_release(struct wl_listener *listener, void *data)
{
    struct mbs_buffer *buffer = wl_container_of(listener, buffer, release);

    (void)data;
    if (buffer->resource)
        wl_buffer_send_release(buffer->resource);
}

/* ---- 协议请求 ---- */
static void mbs_create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                              int32_t width, int32_t height, uint32_t stride, uint32_t format,
                              uint32_t unk1, uint32_t unk2, int32_t fd)
{
    if (g_logged_buffers < 6) {
        g_logged_buffers++;
        LOG("create_buffer %dx%d stride=%u format=%.4s(0x%08x) unk1=0x%x unk2=0x%x fd=%d\n", width,
            height, stride, (const char *)&format, format, unk1, unk2, fd);
    }
    if (width <= 0 || height <= 0 || fd < 0) {
        if (fd >= 0)
            close(fd);
        wl_resource_post_error(resource, 0, "invalid buffer %dx%d", width, height);
        return;
    }

    struct mbs_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        close(fd);
        wl_resource_post_no_memory(resource);
        return;
    }
    W.buffer_init(&buffer->base, &mbs_buffer_impl, width, height);

    buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer->resource) {
        W.buffer_finish(&buffer->base);
        free(buffer);
        close(fd);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(buffer->resource, &wl_buffer_impl, buffer,
                                   wl_buffer_resource_destroyed);

    /*
     * unk1/unk2 实测是 DRM modifier 的高/低 32 位：r23p0 默认给 0x08000000/0x71，
     * 即 0x0800000000000071 = ARM AFBC（16x16 | YTR | SPLIT | SPARSE）。
     * 必须原样交给 EGL 导入，否则会被当成线性布局读成花屏。两者都为 0 时按隐式布局处理。
     */
    uint64_t modifier = ((uint64_t)unk1 << 32) | unk2;

    buffer->dmabuf = (struct wlr_dmabuf_attributes){
        .width     = width,
        .height    = height,
        .format    = format,
        .modifier  = modifier ? modifier : DRM_FORMAT_MOD_INVALID,
        .n_planes  = 1,
        .offset[0] = 0,
        .stride[0] = stride,
        .fd        = { fd, -1, -1, -1 },
    };

    buffer->release.notify = mbs_buffer_handle_release;
    wl_signal_add(&buffer->base.events.release, &buffer->release);
}

/*
 * 早期从 backend 拿到的 fd 可能随后被关闭、号被复用（实测会这样），所以每次用前都校验：
 * 必须仍是 DRM 设备且是 master；否则扫描 /proc/self/fd 里的 /dev/dri/card*，挑 drmIsMaster 的那个。
 */
static bool fd_is_drm_master(int fd)
{
    char path[64], target[64];
    ssize_t n;

    if (fd < 0)
        return false;
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    n = readlink(path, target, sizeof target - 1);
    if (n <= 0)
        return false;
    target[n] = 0;
    return strncmp(target, "/dev/dri/card", 13) == 0 && drmIsMaster(fd);
}

static int get_master_fd(void)
{
    if (fd_is_drm_master(g_master_fd))
        return g_master_fd;

    DIR *dir = opendir("/proc/self/fd");
    struct dirent *e;
    int found = -1;

    while (dir && (e = readdir(dir))) {
        int fd = atoi(e->d_name);
        if (e->d_name[0] != '.' && fd != dirfd(dir) && fd_is_drm_master(fd)) {
            found = fd;
            break;
        }
    }
    if (dir)
        closedir(dir);
    if (found != g_master_fd)
        LOG("DRM master fd 由 %d 改为 %d\n", g_master_fd, found);
    g_master_fd = found;
    return found;
}

static void mbs_auth(struct wl_client *client, struct wl_resource *resource, uint32_t magic)
{
    (void)client;
    (void)resource;
    int fd = get_master_fd();

    if (fd < 0) {
        LOG("auth(%u)：找不到 DRM master fd，忽略\n", magic);
        return;
    }
    int ret = drmAuthMagic(fd, magic);
    if (ret)
        LOG("auth(%u) 失败：%d\n", magic, ret);
}

static const struct mbs_request_impl mbs_impl = {
    .create_buffer = mbs_create_buffer,
    .auth          = mbs_auth,
};

static void mbs_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *resource = wl_resource_create(client, &mbs_interface, version, id);

    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &mbs_impl, NULL, NULL);

    const char *dev = getenv("TL_MALIBUF_DEVICE");
    char       *bus = NULL;

    if (!dev) {
        int fd = get_master_fd();
        bus = fd >= 0 ? drmGetBusid(fd) : NULL;
        dev = bus ? bus : "";
    }
    wl_resource_post_event(resource, MBS_ALLOC_DEVICE, dev);

    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, NULL, NULL);
    LOG("客户端 pid=%d 绑定 v%u，alloc_device=\"%s\"\n", pid, version, dev);
    if (bus)
        drmFreeBusid(bus);
}

/* ---- 挂钩点 ---- */
struct wlr_allocator *wlr_allocator_autocreate(struct wlr_backend *backend,
                                               struct wlr_renderer *renderer)
{
    static struct wlr_allocator *(*real)(struct wlr_backend *, struct wlr_renderer *);

    if (!real)
        *(void **)&real = dlsym(RTLD_NEXT, "wlr_allocator_autocreate");
    if (g_master_fd < 0 && resolve_wlr()) {
        g_master_fd = W.backend_get_drm_fd(backend);
        LOG("DRM master fd=%d\n", g_master_fd);
    }
    return real(backend, renderer);
}

struct wlr_drm *wlr_drm_create(struct wl_display *display, struct wlr_renderer *renderer)
{
    static struct wlr_drm *(*real)(struct wl_display *, struct wlr_renderer *);

    if (!real)
        *(void **)&real = dlsym(RTLD_NEXT, "wlr_drm_create");
    struct wlr_drm *drm = real(display, renderer);

    const char *off = getenv("TL_MALIBUF_DISABLE");
    if (g_global_created || (off && *off && *off != '0') || !resolve_wlr())
        return drm;
    if (g_master_fd < 0) {
        g_master_fd = W.renderer_get_drm_fd(renderer);
        LOG("未拿到 backend fd，退而使用 renderer fd=%d（auth 可能失败）\n", g_master_fd);
    }
    if (!wl_global_create(display, &mbs_interface, 5, NULL, mbs_bind)) {
        LOG("wl_global_create 失败\n");
        return drm;
    }
    W.register_resource_interface(&mbs_resource_interface);
    g_global_created = true;
    LOG("已注册 mali_buffer_sharing v5\n");
    return drm;
}
