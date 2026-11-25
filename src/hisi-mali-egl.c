/*
 * libEGL_hisi_mali：把 UOS 的厂商 libmali（Mali-G78 / Valhall r23p0，EGL 1.4 + GLES 3.2）包装成
 * libglvnd 的 EGL vendor，让 deepin 25 上的 Wayland 客户端不改环境变量就走 GPU。
 *
 * 为什么不用 LD_PRELOAD：控制中心等程序经 deepin-security-loader（带 file capability）启动，
 * glibc 在安全模式下会清掉 LD_PRELOAD/LD_LIBRARY_PATH；glvnd vendor 由 libEGL 按 JSON 加载，不受影响。
 *
 * 工作方式：
 *   - /etc/glvnd/egl_vendor.d/10_hisi_mali.json 让 glvnd 先问本 vendor，再问 Mesa。
 *   - 只认领 Wayland 平台的显示，并且要求合成器提供 mali_buffer_sharing（由 treeland 里的 tl-malibuf 提供），
 *     另开私有事件队列枚举 registry 判断；不满足、在黑名单里、或 libmali 初始化失败就返回 EGL_NO_DISPLAY，
 *     glvnd 自动交给 Mesa。GBM/X11/设备平台（Xwayland 的 glamor 等）一律不碰。
 *   - libmali 只在真正认领时才 dlopen（RTLD_LOCAL；它带 DT_SYMBOLIC，内部调用不会被 glvnd 的同名符号截走）。
 *   - libmali 的 wl_egl_window 是私有的 96 字节结构，和 libwayland-egl 的不兼容：创建窗口表面时用 libmali 自己的
 *     wl_egl_window_create 另建一个，并挂上 libwayland-egl 的 resize/destroy 回调做转发，交换后回填 attached 尺寸。
 *   - libmali 依赖 UOS 魔改 libwayland-client 的 wl_proxy_ref/unref/get_refcount，本库实现并在加载 libmali 前
 *     把自己提升为 RTLD_GLOBAL 供其解析（其余符号均为 hidden）。
 *   - libmali 只有 OpenGL ES：选 config 时把 EGL_OPENGL_BIT 换成 EGL_OPENGL_ES2_BIT（deepin 的 Qt6 是桌面 GL 版，
 *     据此自动改走 GLES）；当前绑定的 API 是桌面 OpenGL 时拒绝建上下文，让 GTK4、mpv 等自行回退 GLES。
 *   - EGL 1.5 的 eglCreateImage/eglCreateSync 等转调 KHR 版本。
 *
 * 开关：HISI_MALI_DISABLE=1（单个程序不用）；/etc/hisi-mali/egl-off 存在时全局不用；
 *       /etc/hisi-mali/egl-deny 每行一个程序名或完整路径（# 开头为注释）；HISI_MALI_DEBUG=1 打印决策。
 * 编译：gcc -O2 -Wall -shared -fPIC -fvisibility=hidden -o libEGL_hisi_mali.so.0 hisi-mali-egl.c \
 *         -Wl,-soname,libEGL_hisi_mali.so.0 -lwayland-client -ldl -lpthread
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glvnd/libeglabi.h>
#include <wayland-client.h>

#define PUBLIC     __attribute__((visibility("default")))
#define MALI_PATH  "/usr/local/lib/hisi-mali/libmali.so.0"
#define OFF_FILE   "/etc/hisi-mali/egl-off"
#define DENY_FILE  "/etc/hisi-mali/egl-deny"

PUBLIC const char hisi_mali_egl_version[] = "hisi-mali-egl v1";

static const __EGLapiExports *g_exports;
static int g_debug;

#define DBG(...)                                                    \
    do {                                                            \
        if (g_debug) {                                              \
            fprintf(stderr, "hisi-mali-egl[%d]: ", (int)getpid());  \
            fprintf(stderr, __VA_ARGS__);                           \
        }                                                           \
    } while (0)

/* ---------------- wl_proxy_ref/unref/get_refcount（与 mali-compat 相同的实现） ---------------- */
struct wl_proxy_layout {
    const void *interface;
    const void *implementation;
    uint32_t    id;
    void       *display;
    void       *queue;
    uint32_t    flags;
    int         refcount;
    void       *user_data;
    void       *dispatcher;
    uint32_t    version;
};
_Static_assert(__builtin_offsetof(struct wl_proxy_layout, flags) == 40, "flags");
_Static_assert(__builtin_offsetof(struct wl_proxy_layout, refcount) == 44, "refcount");
_Static_assert(__builtin_offsetof(struct wl_proxy_layout, version) == 64, "version");
#define WL_PROXY_FLAG_DESTROYED (1 << 1)

static int proxy_layout_ok(struct wl_proxy *p)
{
    static int state; /* 0 未检查，1 可用，-1 不可用 */

    if (state == 0) {
        const struct wl_proxy_layout *l = (const void *)p;

        state = (wl_proxy_get_version(p) == l->version && l->refcount > 0 && l->refcount < 1000) ? 1 : -1;
        if (state < 0)
            fprintf(stderr, "hisi-mali-egl: wl_proxy 布局校验失败，引用计数退回空实现\n");
    }
    return state > 0;
}

PUBLIC void wl_proxy_ref(struct wl_proxy *p)
{
    if (p && proxy_layout_ok(p))
        __atomic_add_fetch(&((struct wl_proxy_layout *)p)->refcount, 1, __ATOMIC_SEQ_CST);
}

PUBLIC void wl_proxy_unref(struct wl_proxy *p)
{
    struct wl_proxy_layout *l = (void *)p;

    if (!p || !proxy_layout_ok(p))
        return;
    if (__atomic_sub_fetch(&l->refcount, 1, __ATOMIC_SEQ_CST) > 0)
        return;
    if (l->flags & WL_PROXY_FLAG_DESTROYED)
        free(p);
}

PUBLIC int wl_proxy_get_refcount(struct wl_proxy *p)
{
    return (p && proxy_layout_ok(p)) ? ((struct wl_proxy_layout *)p)->refcount : 1;
}

/* ---------------- 加载 libmali ---------------- */
static void *g_mali;
static pthread_once_t g_mali_once = PTHREAD_ONCE_INIT;

static struct {
    __eglMustCastToProperFunctionPointerType (*GetProcAddress)(const char *);
    EGLDisplay (*GetDisplay)(EGLNativeDisplayType);
    EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*Terminate)(EGLDisplay);
    EGLint (*GetError)(void);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapBuffersWithDamageKHR)(EGLDisplay, EGLSurface, const EGLint *, EGLint);
    EGLBoolean (*SwapBuffersWithDamageEXT)(EGLDisplay, EGLSurface, const EGLint *, EGLint);
    const char *(*QueryString)(EGLDisplay, EGLint);
    EGLBoolean (*BindAPI)(EGLenum);
    EGLBoolean (*ReleaseThread)(void);
    EGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (*DestroyImageKHR)(EGLDisplay, EGLImageKHR);
    EGLSyncKHR (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
    EGLBoolean (*DestroySyncKHR)(EGLDisplay, EGLSyncKHR);
    EGLint (*ClientWaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);
    EGLBoolean (*GetSyncAttribKHR)(EGLDisplay, EGLSyncKHR, EGLint, EGLint *);
    EGLint (*WaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint);
    void *(*wl_create)(struct wl_surface *, int, int);
    void (*wl_destroy)(void *);
    void (*wl_resize)(void *, int, int, int, int);
    void (*wl_get_attached_size)(void *, int *, int *);
} M;

static void *mali_sym(const char *name)
{
    void *p;

    if (!g_mali)
        return NULL;
    p = dlsym(g_mali, name);
    if (!p && M.GetProcAddress)
        p = (void *)M.GetProcAddress(name);
    return p;
}

PUBLIC EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports *exports, __EGLvendorInfo *vendor,
                             __EGLapiImports *imports);

static void load_mali(void)
{
    Dl_info di;
    void   *h;

    /* 让 libmali 能解析到本库的 wl_proxy_ref 等（必须在 dlopen libmali 之前，它是 BIND_NOW） */
    if (dladdr((void *)&__egl_Main, &di) && di.dli_fname)
        dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
    h = dlopen(MALI_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        DBG("加载 libmali 失败：%s\n", dlerror());
        return;
    }
    g_mali = h;
#define S(field, name) *(void **)&M.field = dlsym(h, name)
    S(GetProcAddress, "eglGetProcAddress");
    S(GetDisplay, "eglGetDisplay");
    S(Initialize, "eglInitialize");
    S(Terminate, "eglTerminate");
    S(GetError, "eglGetError");
    S(ChooseConfig, "eglChooseConfig");
    S(CreateContext, "eglCreateContext");
    S(CreateWindowSurface, "eglCreateWindowSurface");
    S(DestroySurface, "eglDestroySurface");
    S(SwapBuffers, "eglSwapBuffers");
    S(QueryString, "eglQueryString");
    S(BindAPI, "eglBindAPI");
    S(ReleaseThread, "eglReleaseThread");
    S(wl_create, "wl_egl_window_create");
    S(wl_destroy, "wl_egl_window_destroy");
    S(wl_resize, "wl_egl_window_resize");
    S(wl_get_attached_size, "wl_egl_window_get_attached_size");
#undef S
#define P(field, name) *(void **)&M.field = mali_sym(name)
    P(GetPlatformDisplayEXT, "eglGetPlatformDisplayEXT");
    P(SwapBuffersWithDamageKHR, "eglSwapBuffersWithDamageKHR");
    P(SwapBuffersWithDamageEXT, "eglSwapBuffersWithDamageEXT");
    P(CreateImageKHR, "eglCreateImageKHR");
    P(DestroyImageKHR, "eglDestroyImageKHR");
    P(CreateSyncKHR, "eglCreateSyncKHR");
    P(DestroySyncKHR, "eglDestroySyncKHR");
    P(ClientWaitSyncKHR, "eglClientWaitSyncKHR");
    P(GetSyncAttribKHR, "eglGetSyncAttribKHR");
    P(WaitSyncKHR, "eglWaitSyncKHR");
#undef P
    if (!M.GetDisplay || !M.Initialize || !M.CreateWindowSurface || !M.wl_create || !M.wl_destroy ||
        !M.wl_resize || !M.wl_get_attached_size) {
        DBG("libmali 缺少必要符号，放弃\n");
        g_mali = NULL; /* 不 dlclose：可能已有指针被取走 */
    }
}

/* ---------------- 错误码：本库自己产生的错误优先返回 ---------------- */
static __thread EGLint t_err;

static void set_err(EGLint e)
{
    t_err = e;
}

/* ---------------- 是否认领：进程策略 + 合成器探测 ---------------- */
static int list_has(const char *list_path, const char *exe, const char *base)
{
    FILE *f = fopen(list_path, "r");
    char  line[512];
    int   hit = 0;

    if (!f)
        return 0;
    while (!hit && fgets(line, sizeof line, f)) {
        char *s = line, *e;

        while (*s == ' ' || *s == '\t')
            s++;
        e = s + strcspn(s, "#\r\n");
        while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        *e = 0;
        if (*s && (strcmp(s, exe) == 0 || strcmp(s, base) == 0))
            hit = 1;
    }
    fclose(f);
    return hit;
}

static int process_allowed(void)
{
    static int state; /* 0 未判断，1 允许，-1 拒绝 */
    /* 有自己的 GPU 进程/沙箱或自带 GL 栈的程序，以及合成器本身，默认不碰 */
    static const char *const builtin_deny[] = {
        "treeland", "Xwayland", "kwin_wayland", "firefox", "firefox-esr", "firefox-bin",
        "chromium", "chrome", "google-chrome", "msedge", "electron", "code", "QtWebEngineProcess",
        NULL,
    };
    char        exe[512];
    const char *base, *off;
    ssize_t     n;

    if (state)
        return state > 0;
    state = -1;
    off = getenv("HISI_MALI_DISABLE");
    if (off && *off && *off != '0')
        return 0;
    if (access(OFF_FILE, F_OK) == 0)
        return 0;
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return 0;
    exe[n] = 0;
    base = strrchr(exe, '/') ? strrchr(exe, '/') + 1 : exe;
    for (int i = 0; builtin_deny[i]; i++)
        if (strcmp(base, builtin_deny[i]) == 0)
            return 0;
    if (list_has(DENY_FILE, exe, base))
        return 0;
    state = 1;
    return 1;
}

static void probe_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                         uint32_t version)
{
    (void)reg, (void)name, (void)version;
    if (strcmp(iface, "mali_buffer_sharing") == 0)
        *(int *)data = 1;
}

static void probe_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data, (void)reg, (void)name;
}

static const struct wl_registry_listener probe_listener = { probe_global, probe_global_remove };

/* 在程序自己的 wl_display 上用私有事件队列枚举 registry（与 Mesa 的做法相同，不干扰程序的事件分发） */
static int display_has_mali(struct wl_display *dpy)
{
    struct wl_event_queue *q = wl_display_create_queue(dpy);
    struct wl_display     *wrapper;
    struct wl_registry    *reg;
    int                    found = 0;

    if (!q)
        return 0;
    wrapper = wl_proxy_create_wrapper(dpy);
    if (!wrapper) {
        wl_event_queue_destroy(q);
        return 0;
    }
    wl_proxy_set_queue((struct wl_proxy *)wrapper, q);
    reg = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (reg) {
        wl_registry_add_listener(reg, &probe_listener, &found);
        if (wl_display_roundtrip_queue(dpy, q) < 0)
            found = 0;
        wl_registry_destroy(reg);
    }
    wl_event_queue_destroy(q);
    return found;
}

/* ---------------- vendor 导入函数 ---------------- */
static EGLDisplay vnd_getPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attrs)
{
    EGLDisplay d;
    EGLint     maj, min;

    (void)attrs;
    if (platform != EGL_PLATFORM_WAYLAND_KHR || !native)
        return EGL_NO_DISPLAY;
    if (!process_allowed())
        return EGL_NO_DISPLAY;
    if (!display_has_mali(native)) {
        DBG("合成器没有 mali_buffer_sharing，交给 Mesa\n");
        return EGL_NO_DISPLAY;
    }
    pthread_once(&g_mali_once, load_mali);
    if (!g_mali)
        return EGL_NO_DISPLAY;
    d = M.GetPlatformDisplayEXT ? M.GetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, native, NULL)
                                : M.GetDisplay((EGLNativeDisplayType)native);
    if (d == EGL_NO_DISPLAY) {
        DBG("libmali 拿不到显示，交给 Mesa\n");
        return EGL_NO_DISPLAY;
    }
    /* 先试初始化：一旦返回给 glvnd 就没法再换 vendor，失败要在这里拦住（重复 eglInitialize 是无害的） */
    if (!M.Initialize(d, &maj, &min)) {
        DBG("libmali eglInitialize 失败（0x%x），交给 Mesa\n", M.GetError ? M.GetError() : 0);
        return EGL_NO_DISPLAY;
    }
    DBG("认领 Wayland 显示，libmali EGL %d.%d\n", maj, min);
    return d;
}

static EGLBoolean vnd_getSupportsAPI(EGLenum api)
{
    return api == EGL_OPENGL_ES_API;
}

static const char CLIENT_EXTS[] = "EGL_EXT_client_extensions EGL_EXT_platform_base "
                                  "EGL_KHR_platform_wayland EGL_EXT_platform_wayland";

static const char *vnd_getVendorString(int name)
{
    return name == __EGL_VENDOR_STRING_PLATFORM_EXTENSIONS ? CLIENT_EXTS : NULL;
}

/* ---------------- 窗口转换：libwayland-egl 的 wl_egl_window -> libmali 自己的 ---------------- */
struct std_wl_egl_window { /* wayland-egl-backend.h，WL_EGL_WINDOW_VERSION 3 */
    const intptr_t version;
    int            width, height, dx, dy;
    int            attached_width, attached_height;
    void          *driver_private;
    void (*resize_callback)(struct std_wl_egl_window *, void *);
    void (*destroy_window_callback)(void *);
    struct wl_surface *surface;
};

struct win_map {
    struct win_map           *next;
    struct std_wl_egl_window *std; /* 程序的窗口；程序先销毁窗口时置 NULL */
    void                     *mw;  /* libmali 的窗口 */
    EGLDisplay                dpy;
    EGLSurface                surf;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct win_map *g_maps;

static void std_resize_cb(struct std_wl_egl_window *std, void *priv)
{
    struct win_map *m = priv;

    if (m && m->mw)
        M.wl_resize(m->mw, std->width, std->height, std->dx, std->dy);
}

static void std_destroy_cb(void *priv)
{
    struct win_map *m = priv;

    pthread_mutex_lock(&g_lock);
    if (m)
        m->std = NULL;
    pthread_mutex_unlock(&g_lock);
}

static void detach_std(struct win_map *m)
{
    pthread_mutex_lock(&g_lock);
    if (m->std) {
        m->std->driver_private          = NULL;
        m->std->resize_callback         = NULL;
        m->std->destroy_window_callback = NULL;
        m->std                          = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}

static struct win_map *wrap_window(EGLDisplay dpy, void *native)
{
    struct std_wl_egl_window *std = native;
    struct win_map           *m;

    if (!std || std->version < 3 || std->version > 64 || !std->surface) {
        set_err(EGL_BAD_NATIVE_WINDOW);
        return NULL;
    }
    if (std->driver_private) {
        set_err(EGL_BAD_ALLOC);
        return NULL;
    }
    m = calloc(1, sizeof *m);
    if (!m) {
        set_err(EGL_BAD_ALLOC);
        return NULL;
    }
    m->mw = M.wl_create(std->surface, std->width, std->height);
    if (!m->mw) {
        free(m);
        set_err(EGL_BAD_NATIVE_WINDOW);
        return NULL;
    }
    m->std                       = std;
    m->dpy                       = dpy;
    std->driver_private          = m;
    std->resize_callback         = std_resize_cb;
    std->destroy_window_callback = std_destroy_cb;
    return m;
}

static void release_map(struct win_map *m)
{
    detach_std(m);
    M.wl_destroy(m->mw);
    free(m);
}

static EGLSurface create_window_surface(EGLDisplay dpy, EGLConfig cfg, void *native, const EGLint *attrs)
{
    struct win_map *m;
    EGLSurface      s;

    if (!g_mali)
        return EGL_NO_SURFACE;
    m = wrap_window(dpy, native);
    if (!m)
        return EGL_NO_SURFACE;
    s = M.CreateWindowSurface(dpy, cfg, (EGLNativeWindowType)m->mw, attrs);
    if (s == EGL_NO_SURFACE) {
        release_map(m);
        return EGL_NO_SURFACE;
    }
    m->surf = s;
    pthread_mutex_lock(&g_lock);
    m->next = g_maps;
    g_maps  = m;
    pthread_mutex_unlock(&g_lock);
    return s;
}

static struct win_map *take_map(EGLDisplay dpy, EGLSurface s, int all_of_display)
{
    struct win_map **pp, *m, *taken = NULL;

    pthread_mutex_lock(&g_lock);
    for (pp = &g_maps; (m = *pp);) {
        if (m->dpy == dpy && (all_of_display || m->surf == s)) {
            *pp     = m->next;
            m->next = taken;
            taken   = m;
            if (!all_of_display)
                break;
        } else {
            pp = &m->next;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return taken;
}

static void after_swap(EGLDisplay dpy, EGLSurface s)
{
    struct win_map *m;

    pthread_mutex_lock(&g_lock);
    for (m = g_maps; m; m = m->next) {
        if (m->surf == s && m->dpy == dpy) {
            if (m->std) {
                int w = 0, h = 0;

                M.wl_get_attached_size(m->mw, &w, &h);
                m->std->attached_width  = w;
                m->std->attached_height = h;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* ---------------- 属性表转换 ---------------- */
static EGLint *attrib_to_int(const EGLAttrib *attrs, EGLint *small, size_t cap)
{
    size_t  n = 0;
    EGLint *a = small;

    if (!attrs)
        return NULL;
    while (attrs[n] != EGL_NONE)
        n += 2;
    if (n + 1 > cap)
        a = malloc((n + 1) * sizeof *a);
    if (!a)
        return NULL;
    for (size_t i = 0; i < n; i++)
        a[i] = (EGLint)attrs[i];
    a[n] = EGL_NONE;
    return a;
}

#define FREE_ATTRS(a, small)          \
    do {                              \
        if ((a) && (a) != (small))    \
            free(a);                  \
    } while (0)

/* ---------------- 需要特殊处理的 EGL 入口 ---------------- */
static EGLint w_eglGetError(void)
{
    EGLint e = t_err;

    if (e) {
        t_err = 0;
        return e;
    }
    return (g_mali && M.GetError) ? M.GetError() : EGL_SUCCESS;
}

static const char *w_eglQueryString(EGLDisplay dpy, EGLint name)
{
    if (dpy == EGL_NO_DISPLAY)
        return name == EGL_EXTENSIONS ? CLIENT_EXTS : NULL;
    return (g_mali && M.QueryString) ? M.QueryString(dpy, name) : NULL;
}

static EGLBoolean w_eglBindAPI(EGLenum api)
{
    /* glvnd 会对所有 vendor 广播；libmali 只支持 GLES，桌面 GL 不转发，避免在 libmali 里留下错误码 */
    if (api == EGL_OPENGL_ES_API && g_mali && M.BindAPI)
        return M.BindAPI(api);
    return EGL_TRUE;
}

static EGLBoolean w_eglReleaseThread(void)
{
    return (g_mali && M.ReleaseThread) ? M.ReleaseThread() : EGL_TRUE;
}

static EGLBoolean w_eglTerminate(EGLDisplay dpy)
{
    EGLBoolean      r;
    struct win_map *m, *next;

    if (!g_mali)
        return EGL_FALSE;
    r = M.Terminate(dpy);
    for (m = take_map(dpy, EGL_NO_SURFACE, 1); m; m = next) {
        next = m->next;
        release_map(m);
    }
    return r;
}

#define EGL_RENDERABLE_TYPE_ 0x3040
static EGLBoolean w_eglChooseConfig(EGLDisplay dpy, const EGLint *attrs, EGLConfig *configs, EGLint size,
                                    EGLint *num)
{
    EGLint     small[128], *a = small;
    size_t     n = 0;
    EGLBoolean r;

    if (!g_mali)
        return EGL_FALSE;
    if (!attrs)
        return M.ChooseConfig(dpy, attrs, configs, size, num);
    while (attrs[n] != EGL_NONE)
        n += 2;
    if (n + 1 > sizeof small / sizeof small[0])
        a = malloc((n + 1) * sizeof *a);
    if (!a)
        return EGL_FALSE;
    memcpy(a, attrs, (n + 1) * sizeof *a);
    for (size_t i = 0; i < n; i += 2)
        if (a[i] == EGL_RENDERABLE_TYPE_ && (a[i + 1] & EGL_OPENGL_BIT))
            a[i + 1] = (a[i + 1] & ~EGL_OPENGL_BIT) | EGL_OPENGL_ES2_BIT;
    r = M.ChooseConfig(dpy, a, configs, size, num);
    FREE_ATTRS(a, small);
    return r;
}

static EGLContext w_eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attrs)
{
    if (!g_mali)
        return EGL_NO_CONTEXT;
    if (g_exports && g_exports->getCurrentApi && g_exports->getCurrentApi() != EGL_OPENGL_ES_API) {
        DBG("拒绝桌面 OpenGL 上下文（libmali 只有 GLES），等程序回退 GLES\n");
        set_err(EGL_BAD_MATCH);
        return EGL_NO_CONTEXT;
    }
    return M.CreateContext(dpy, cfg, share, attrs);
}

static EGLSurface w_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win,
                                           const EGLint *attrs)
{
    return create_window_surface(dpy, cfg, (void *)win, attrs);
}

static EGLSurface w_eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig cfg, void *win,
                                                      const EGLint *attrs)
{
    return create_window_surface(dpy, cfg, win, attrs);
}

static EGLSurface w_eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig cfg, void *win,
                                                   const EGLAttrib *attrs)
{
    EGLint     small[32], *a = attrib_to_int(attrs, small, 32);
    EGLSurface s = create_window_surface(dpy, cfg, win, a);

    FREE_ATTRS(a, small);
    return s;
}

static EGLBoolean w_eglDestroySurface(EGLDisplay dpy, EGLSurface s)
{
    EGLBoolean      r;
    struct win_map *m;

    if (!g_mali)
        return EGL_FALSE;
    r = M.DestroySurface(dpy, s);
    m = take_map(dpy, s, 0);
    if (m)
        release_map(m); /* libmali 的窗口有引用计数，表面若仍是当前表面会延后释放 */
    return r;
}

static EGLBoolean w_eglSwapBuffers(EGLDisplay dpy, EGLSurface s)
{
    EGLBoolean r;

    if (!g_mali)
        return EGL_FALSE;
    r = M.SwapBuffers(dpy, s);
    after_swap(dpy, s);
    return r;
}

static EGLBoolean w_eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface s, const EGLint *rects, EGLint n)
{
    EGLBoolean r;

    if (!M.SwapBuffersWithDamageKHR)
        return w_eglSwapBuffers(dpy, s);
    r = M.SwapBuffersWithDamageKHR(dpy, s, rects, n);
    after_swap(dpy, s);
    return r;
}

static EGLBoolean w_eglSwapBuffersWithDamageEXT(EGLDisplay dpy, EGLSurface s, const EGLint *rects, EGLint n)
{
    EGLBoolean r;

    if (!M.SwapBuffersWithDamageEXT)
        return w_eglSwapBuffersWithDamageKHR(dpy, s, rects, n);
    r = M.SwapBuffersWithDamageEXT(dpy, s, rects, n);
    after_swap(dpy, s);
    return r;
}

static EGLImage w_eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buf,
                                 const EGLAttrib *attrs)
{
    EGLint   small[64], *a;
    EGLImage r;

    if (!M.CreateImageKHR) {
        set_err(EGL_BAD_DISPLAY);
        return EGL_NO_IMAGE;
    }
    a = attrib_to_int(attrs, small, 64);
    r = M.CreateImageKHR(dpy, ctx, target, buf, a);
    FREE_ATTRS(a, small);
    return r;
}

static EGLBoolean w_eglDestroyImage(EGLDisplay dpy, EGLImage img)
{
    return M.DestroyImageKHR ? M.DestroyImageKHR(dpy, img) : EGL_FALSE;
}

static EGLSync w_eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib *attrs)
{
    EGLint  small[32], *a;
    EGLSync r;

    if (!M.CreateSyncKHR) {
        set_err(EGL_BAD_DISPLAY);
        return EGL_NO_SYNC;
    }
    a = attrib_to_int(attrs, small, 32);
    r = M.CreateSyncKHR(dpy, type, a);
    FREE_ATTRS(a, small);
    return r;
}

static EGLBoolean w_eglDestroySync(EGLDisplay dpy, EGLSync sync)
{
    return M.DestroySyncKHR ? M.DestroySyncKHR(dpy, sync) : EGL_FALSE;
}

static EGLint w_eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout)
{
    return M.ClientWaitSyncKHR ? M.ClientWaitSyncKHR(dpy, sync, flags, timeout) : EGL_FALSE;
}

static EGLBoolean w_eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attr, EGLAttrib *value)
{
    EGLint     v = 0;
    EGLBoolean r;

    if (!M.GetSyncAttribKHR)
        return EGL_FALSE;
    r = M.GetSyncAttribKHR(dpy, sync, attr, &v);
    if (r && value)
        *value = v;
    return r;
}

static EGLBoolean w_eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags)
{
    return M.WaitSyncKHR ? (EGLBoolean)M.WaitSyncKHR(dpy, sync, flags) : EGL_FALSE;
}

/* ---------------- 其余 EGL 1.4 核心函数：直接转给 libmali ---------------- */
#define FWD(ret, fail, name, params, args)                  \
    static ret w_##name params                              \
    {                                                       \
        static ret(*f) params;                              \
        if (!f) {                                           \
            *(void **)&f = mali_sym(#name);                 \
            if (!f)                                         \
                return fail;                                \
        }                                                   \
        return f args;                                      \
    }

FWD(EGLBoolean, EGL_FALSE, eglInitialize, (EGLDisplay d, EGLint *a, EGLint *b), (d, a, b))
FWD(EGLBoolean, EGL_FALSE, eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint s, EGLint *n), (d, c, s, n))
FWD(EGLBoolean, EGL_FALSE, eglGetConfigAttrib, (EGLDisplay d, EGLConfig c, EGLint a, EGLint *v), (d, c, a, v))
FWD(EGLBoolean, EGL_FALSE, eglCopyBuffers, (EGLDisplay d, EGLSurface s, EGLNativePixmapType p), (d, s, p))
FWD(EGLBoolean, EGL_FALSE, eglDestroyContext, (EGLDisplay d, EGLContext c), (d, c))
FWD(EGLSurface, EGL_NO_SURFACE, eglCreatePbufferSurface, (EGLDisplay d, EGLConfig c, const EGLint *a),
    (d, c, a))
FWD(EGLSurface, EGL_NO_SURFACE, eglCreatePixmapSurface,
    (EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint *a), (d, c, p, a))
FWD(EGLBoolean, EGL_FALSE, eglMakeCurrent, (EGLDisplay d, EGLSurface w, EGLSurface r, EGLContext c),
    (d, w, r, c))
FWD(EGLBoolean, EGL_FALSE, eglQueryContext, (EGLDisplay d, EGLContext c, EGLint a, EGLint *v), (d, c, a, v))
FWD(EGLBoolean, EGL_FALSE, eglQuerySurface, (EGLDisplay d, EGLSurface s, EGLint a, EGLint *v), (d, s, a, v))
FWD(EGLBoolean, EGL_FALSE, eglWaitGL, (void), ())
FWD(EGLBoolean, EGL_FALSE, eglWaitNative, (EGLint e), (e))
FWD(EGLBoolean, EGL_FALSE, eglWaitClient, (void), ())
FWD(EGLBoolean, EGL_FALSE, eglBindTexImage, (EGLDisplay d, EGLSurface s, EGLint b), (d, s, b))
FWD(EGLBoolean, EGL_FALSE, eglReleaseTexImage, (EGLDisplay d, EGLSurface s, EGLint b), (d, s, b))
FWD(EGLBoolean, EGL_FALSE, eglSurfaceAttrib, (EGLDisplay d, EGLSurface s, EGLint a, EGLint v), (d, s, a, v))
FWD(EGLBoolean, EGL_FALSE, eglSwapInterval, (EGLDisplay d, EGLint i), (d, i))
FWD(EGLSurface, EGL_NO_SURFACE, eglCreatePbufferFromClientBuffer,
    (EGLDisplay d, EGLenum t, EGLClientBuffer b, EGLConfig c, const EGLint *a), (d, t, b, c, a))

static const struct {
    const char *name;
    void       *fn;
} g_wrappers[] = {
#define W(n) { #n, (void *)w_##n }
    W(eglGetError), W(eglQueryString), W(eglBindAPI), W(eglReleaseThread), W(eglTerminate),
    W(eglChooseConfig), W(eglCreateContext), W(eglCreateWindowSurface), W(eglCreatePlatformWindowSurface),
    W(eglCreatePlatformWindowSurfaceEXT), W(eglDestroySurface), W(eglSwapBuffers),
    W(eglSwapBuffersWithDamageKHR), W(eglSwapBuffersWithDamageEXT), W(eglCreateImage), W(eglDestroyImage),
    W(eglCreateSync), W(eglDestroySync), W(eglClientWaitSync), W(eglGetSyncAttrib), W(eglWaitSync),
    W(eglInitialize), W(eglGetConfigs), W(eglGetConfigAttrib), W(eglCopyBuffers), W(eglDestroyContext),
    W(eglCreatePbufferSurface), W(eglCreatePixmapSurface), W(eglMakeCurrent), W(eglQueryContext),
    W(eglQuerySurface), W(eglWaitGL), W(eglWaitNative), W(eglWaitClient), W(eglBindTexImage),
    W(eglReleaseTexImage), W(eglSurfaceAttrib), W(eglSwapInterval), W(eglCreatePbufferFromClientBuffer),
#undef W
};

static void *vnd_getProcAddress(const char *name)
{
    for (size_t i = 0; i < sizeof g_wrappers / sizeof g_wrappers[0]; i++)
        if (strcmp(name, g_wrappers[i].name) == 0)
            return g_wrappers[i].fn;
    /* 平台相关的入口不能直通 libmali（它会按自己的窗口结构解读参数） */
    if (strcmp(name, "eglCreatePlatformPixmapSurface") == 0 ||
        strcmp(name, "eglCreatePlatformPixmapSurfaceEXT") == 0 || strcmp(name, "eglGetDisplay") == 0 ||
        strncmp(name, "eglGetPlatformDisplay", 21) == 0 || strncmp(name, "wl_egl_", 7) == 0)
        return NULL;
    return mali_sym(name);
}

static void *vnd_getDispatchAddress(const char *name)
{
    (void)name;
    return NULL; /* 扩展函数的分发桩由 libEGL 与 Mesa vendor 提供，最终仍按显示回到本 vendor */
}

static void vnd_setDispatchIndex(const char *name, int index)
{
    (void)name, (void)index;
}

PUBLIC EGLBoolean __egl_Main(uint32_t version, const __EGLapiExports *exports, __EGLvendorInfo *vendor,
                             __EGLapiImports *imports)
{
    const char *dbg = getenv("HISI_MALI_DEBUG");

    (void)vendor;
    g_debug = dbg && *dbg && *dbg != '0';
    if (EGL_VENDOR_ABI_GET_MAJOR_VERSION(version) != EGL_VENDOR_ABI_MAJOR_VERSION ||
        EGL_VENDOR_ABI_GET_MINOR_VERSION(version) < EGL_VENDOR_ABI_MINOR_VERSION)
        return EGL_FALSE;
    g_exports                  = exports;
    imports->getPlatformDisplay = vnd_getPlatformDisplay;
    imports->getSupportsAPI     = vnd_getSupportsAPI;
    imports->getVendorString    = vnd_getVendorString;
    imports->getProcAddress     = vnd_getProcAddress;
    imports->getDispatchAddress = vnd_getDispatchAddress;
    imports->setDispatchIndex   = vnd_setDispatchIndex;
    return EGL_TRUE;
}
