/*
 * 让 UOS 的厂商 libmali（Mali-G78 / Valhall r23p0，EGL 1.4 + GLES 3.2 + 自带 gbm）在 deepin 25 的 treeland 里可用。
 * 补齐 libmali 与 deepin 用户态之间的接口差：
 *   1) libmali 依赖 UOS 魔改 libwayland-client 的 wl_proxy_ref/unref/get_refcount（上游没有）。
 *      这里按 libwayland 1.2x 的私有布局实现真实引用计数（带运行时校验，失败退回空实现）。
 *   2) wlroots 直接调用 gbm_bo_get_fd_for_plane（libmali 的 gbm 没有）：单平面格式等价于 gbm_bo_get_fd。
 *   3) Qt/waylib 直接链接 EGL 1.5 的 eglCreateImage/eglDestroyImage（libmali 只有 KHR 版本）：转调 KHR 版本。
 *   4) wlroots 的 GLES2 渲染器强制要求 GL_EXT_unpack_subimage；它在 GLES 3.0 起是核心功能，
 *      Mali 在 ES3 上下文里不再单独宣告，这里在 ES3+ 时把它补进扩展串。
 *   5) v4：客户端（dde-shell、控制中心、壁纸进程、mpv 等）启动时先探测合成器有没有 mali_buffer_sharing，
 *      没有就去掉 libmali 相关环境变量、用 Mesa 重新 exec 自己（否则 eglInitialize 失败，Qt 程序起不来）。
 * 编译：gcc -O2 -shared -fPIC -o libmali-compat.so mali-compat.c -L. -l:libmali.so.0 -lwayland-client -ldl
 * 环境变量：HISI_MALI_DISABLE=1 让客户端直接回退 Mesa；HISI_MALI_DEBUG=1 打印探测结果。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

const char hisi_mali_compat_version[] = "mali-compat v4";

struct wl_proxy;
struct gbm_bo;
typedef void    *EGLDisplay, *EGLContext, *EGLImage, *EGLClientBuffer;
typedef intptr_t EGLAttrib;
typedef int32_t  EGLint;
typedef unsigned EGLenum, EGLBoolean, GLenum;
typedef unsigned char GLubyte;

#define EGL_NONE      0x3038
#define GL_VERSION    0x1F02
#define GL_EXTENSIONS 0x1F03

/*
 * 只让白名单里的程序本体用 libmali：它启动后把本垫片和 libmali 目录从环境变量里去掉，
 * 这样它拉起的子进程（Xwayland、X11 程序、普通应用等）仍走原来的 Mesa，不会被 libmali 误伤。
 * glibc 已在启动时缓存了搜索路径，不影响本进程。
 */
#define COMPOSITOR "/usr/bin/treeland"
static const char *const mali_targets[] = {
    COMPOSITOR,
    "/usr/bin/dde-shell",
    "/usr/bin/dde-control-center",
    "/usr/bin/treeland-wallpaper-factory",
    "/usr/bin/mpv",
    "/usr/lib/qt6/bin/qml",
    NULL,
};

/* 从列表型环境变量里去掉含 hisi-mali 的项（seps 为分隔符，sep 为重新拼接用的分隔符） */
static void drop_mali_entries(const char *var, const char *seps, const char *sep)
{
    const char *val = getenv(var);

    if (!val || !(strstr(val, "hisi-mali") || strstr(val, "libmali")))
        return;

    char *copy = strdup(val), *out = calloc(1, strlen(val) + 1), *save = NULL, *tok;

    if (copy && out) {
        for (tok = strtok_r(copy, seps, &save); tok; tok = strtok_r(NULL, seps, &save)) {
            if (strstr(tok, "hisi-mali") || strstr(tok, "libmali"))
                continue;
            if (*out)
                strcat(out, sep);
            strcat(out, tok);
        }
        if (*out)
            setenv(var, out, 1);
        else
            unsetenv(var);
    }
    free(copy);
    free(out);
}

static void scrub_env(void)
{
    drop_mali_entries("LD_LIBRARY_PATH", ":", ":");
    drop_mali_entries("LD_PRELOAD", " :", " ");
}

/* ---- v4：客户端启动时探测合成器是否提供 mali_buffer_sharing ---- */
static int g_found_mbs;

static void probe_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                         uint32_t version)
{
    (void)data, (void)reg, (void)name, (void)version;
    if (strcmp(iface, "mali_buffer_sharing") == 0)
        g_found_mbs = 1;
}

static void probe_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data, (void)reg, (void)name;
}

static const struct wl_registry_listener probe_listener = { probe_global, probe_global_remove };

/*
 * 另开一条连接枚举 registry，不碰程序自己的连接。WAYLAND_SOCKET 是父进程交给程序的一次性 fd，
 * wl_display_connect 会优先消费它，所以探测期间临时移走、结束后放回，改用 WAYLAND_DISPLAY 连接。
 * 返回 1 表示有，0 表示没有或连不上。
 */
static int compositor_has_mali(void)
{
    char *sock = getenv("WAYLAND_SOCKET");
    char *saved = sock ? strdup(sock) : NULL;
    const char *name = getenv("WAYLAND_DISPLAY");
    struct wl_display *dpy;

    if (saved)
        unsetenv("WAYLAND_SOCKET");
    dpy = wl_display_connect(name && *name ? name : "wayland-0");
    if (saved) {
        setenv("WAYLAND_SOCKET", saved, 1);
        free(saved);
    }
    if (!dpy)
        return 0;

    struct wl_registry *reg = wl_display_get_registry(dpy);

    g_found_mbs = 0;
    if (reg) {
        wl_registry_add_listener(reg, &probe_listener, NULL);
        wl_display_roundtrip(dpy);
        wl_registry_destroy(reg);
    }
    wl_display_disconnect(dpy);
    return g_found_mbs;
}

/*
 * glibc 会把 argc/argv/envp 传给 ELF 构造函数。目标客户端在这里决定走 libmali 还是回退：
 *   有 mali_buffer_sharing：清理环境变量（子进程走 Mesa），本进程继续用 libmali；
 *   没有：清理环境变量后 execv 自己，新进程不再预加载 libmali，Qt 等照常走 Mesa。
 * 不在白名单里的程序（中间的 shell、env、systemctl 等）原样放行，不清理也不探测。
 */
__attribute__((constructor)) static void hisi_mali_init(int argc, char **argv, char **envp)
{
    char    exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    int     hit = 0;

    (void)argc, (void)envp;
    if (n <= 0)
        return;
    exe[n] = 0;
    for (int i = 0; mali_targets[i]; i++)
        hit |= strcmp(exe, mali_targets[i]) == 0;
    if (!hit)
        return;
    if (strcmp(exe, COMPOSITOR) == 0) {
        scrub_env();
        return;
    }

    const char *off = getenv("HISI_MALI_DISABLE");
    const char *dbg = getenv("HISI_MALI_DEBUG");
    int         ok = !(off && *off && *off != '0') && compositor_has_mali();

    if (dbg && *dbg && *dbg != '0')
        fprintf(stderr, "mali-compat: %s %s\n", exe, ok ? "使用 libmali" : "回退到 Mesa");
    scrub_env();
    if (ok || !argv || !argv[0])
        return;
    execv("/proc/self/exe", argv);
    fprintf(stderr, "mali-compat: 回退重启 %s 失败：%s\n", exe, strerror(errno));
}

/*
 * UOS 的 libwayland-client 把内部的 wl_proxy_ref/unref 导出了，libmali 作为客户端会用它们
 * 在别处 wl_proxy_destroy 之后继续持有 proxy。上游这两个是 static，这里按 libwayland 1.2x 的
 * 私有布局实现：flags 在 +40（DESTROYED = 1<<1），refcount 在 +44，version 在 +64。
 * 首次调用时用公开的 wl_proxy_get_version 校验 +64，不一致就退回空实现（最坏是泄漏，不会踩内存）。
 */
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
        uint32_t (*get_version)(struct wl_proxy *) = dlsym(RTLD_DEFAULT, "wl_proxy_get_version");
        const struct wl_proxy_layout *l = (const void *)p;

        state = (get_version && get_version(p) == l->version && l->refcount > 0 && l->refcount < 1000)
                    ? 1 : -1;
        if (state < 0)
            fprintf(stderr, "mali-compat: wl_proxy 布局校验失败，wl_proxy_ref/unref 退回空实现\n");
    }
    return state > 0;
}

void wl_proxy_ref(struct wl_proxy *p)
{
    if (p && proxy_layout_ok(p))
        __atomic_add_fetch(&((struct wl_proxy_layout *)p)->refcount, 1, __ATOMIC_SEQ_CST);
}

void wl_proxy_unref(struct wl_proxy *p)
{
    struct wl_proxy_layout *l = (void *)p;

    if (!p || !proxy_layout_ok(p))
        return;
    if (__atomic_sub_fetch(&l->refcount, 1, __ATOMIC_SEQ_CST) > 0)
        return;
    if (l->flags & WL_PROXY_FLAG_DESTROYED)
        free(p);
}

int wl_proxy_get_refcount(struct wl_proxy *p)
{
    return (p && proxy_layout_ok(p)) ? ((struct wl_proxy_layout *)p)->refcount : 1;
}

int gbm_bo_get_fd(struct gbm_bo *bo);
int gbm_bo_get_plane_count(struct gbm_bo *bo);

int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    if (plane == 0)
        return gbm_bo_get_fd(bo);
    errno = EINVAL;
    return -1;
}

void *eglGetProcAddress(const char *name);

/*
 * deepin 的 Qt6 按桌面 OpenGL 编译，默认要 EGL_OPENGL_BIT 的 config；libmali 只有 OpenGL ES。
 * 把选 config 时的 EGL_OPENGL_BIT 换成 EGL_OPENGL_ES2_BIT：Qt 拿到的 config 不含桌面 GL 位，
 * 会自动把上下文类型定为 OpenGLES（q_glFormatFromConfig），RHI 运行时走 GLES 路径和 GLSL ES 着色器。
 */
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_OPENGL_BIT      0x0008

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrs, void **configs, EGLint size, EGLint *num)
{
    static EGLBoolean (*real)(EGLDisplay, const EGLint *, void **, EGLint, EGLint *);
    EGLint small[128], *a = small;
    size_t n = 0;
    EGLBoolean r;

    if (!real)
        *(void **) &real = dlsym(RTLD_NEXT, "eglChooseConfig");
    if (!attrs)
        return real(dpy, attrs, configs, size, num);
    while (attrs[n] != EGL_NONE)
        n += 2;
    if (n + 1 > sizeof small / sizeof small[0])
        a = malloc((n + 1) * sizeof *a);
    if (!a)
        return 0;
    memcpy(a, attrs, (n + 1) * sizeof *a);
    for (size_t i = 0; i < n; i += 2)
        if (a[i] == EGL_RENDERABLE_TYPE && (a[i + 1] & EGL_OPENGL_BIT))
            a[i + 1] = (a[i + 1] & ~EGL_OPENGL_BIT) | EGL_OPENGL_ES2_BIT;
    r = real(dpy, a, configs, size, num);
    if (a != small)
        free(a);
    return r;
}

EGLImage eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buf, const EGLAttrib *attrs)
{
    static EGLImage (*khr)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLint  small[64], *a = small;
    size_t  n = 0;
    EGLImage r;

    if (!khr)
        *(void **) &khr = eglGetProcAddress("eglCreateImageKHR");
    if (!khr)
        return NULL;
    if (attrs) {
        while (attrs[n] != EGL_NONE)
            n += 2;
        if (n + 1 > sizeof small / sizeof small[0])
            a = malloc((n + 1) * sizeof *a);
        if (!a)
            return NULL;
        for (size_t i = 0; i < n; i++)
            a[i] = (EGLint) attrs[i];
        a[n] = EGL_NONE;
    }
    r = khr(dpy, ctx, target, buf, attrs ? a : NULL);
    if (a != small)
        free(a);
    return r;
}

/*
 * EGL 1.5 的平台函数（Qt 的 wayland-egl 客户端插件直接链接它们，libmali 只有 EXT 版本），
 * 转调 *EXT 版本，EGLAttrib 属性表转成 EGLint。
 */
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
        a[i] = (EGLint) attrs[i];
    a[n] = EGL_NONE;
    return a;
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attrs)
{
    static EGLDisplay (*ext)(EGLenum, void *, const EGLint *);
    EGLint     small[32], *a;
    EGLDisplay r;

    if (!ext)
        *(void **) &ext = eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!ext)
        return NULL;
    a = attrib_to_int(attrs, small, 32);
    r = ext(platform, native, a);
    if (a && a != small)
        free(a);
    return r;
}

void *eglCreatePlatformWindowSurface(EGLDisplay dpy, void *config, void *win, const EGLAttrib *attrs)
{
    static void *(*ext)(EGLDisplay, void *, void *, const EGLint *);
    EGLint small[32], *a;
    void  *r;

    if (!ext)
        *(void **) &ext = eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    if (!ext)
        return NULL;
    a = attrib_to_int(attrs, small, 32);
    r = ext(dpy, config, win, a);
    if (a && a != small)
        free(a);
    return r;
}

void *eglCreatePlatformPixmapSurface(EGLDisplay dpy, void *config, void *pix, const EGLAttrib *attrs)
{
    static void *(*ext)(EGLDisplay, void *, void *, const EGLint *);
    EGLint small[32], *a;
    void  *r;

    if (!ext)
        *(void **) &ext = eglGetProcAddress("eglCreatePlatformPixmapSurfaceEXT");
    if (!ext)
        return NULL;
    a = attrib_to_int(attrs, small, 32);
    r = ext(dpy, config, pix, a);
    if (a && a != small)
        free(a);
    return r;
}

EGLBoolean eglDestroyImage(EGLDisplay dpy, EGLImage img)
{
    static EGLBoolean (*khr)(EGLDisplay, EGLImage);

    if (!khr)
        *(void **) &khr = eglGetProcAddress("eglDestroyImageKHR");
    return khr ? khr(dpy, img) : 0;
}

const GLubyte *glGetString(GLenum name)
{
    static const GLubyte *(*real)(GLenum);
    static char          *ext_cache;
    static const GLubyte *ext_src;
    const GLubyte        *s;

    if (!real)
        *(void **) &real = dlsym(RTLD_NEXT, "glGetString");
    s = real(name);
    if (name != GL_EXTENSIONS || !s)
        return s;
    if (s == ext_src && ext_cache)
        return (const GLubyte *) ext_cache;
    {
        const char *ver = (const char *) real(GL_VERSION);
        const char *e   = (const char *) s;
        if (ver && strncmp(ver, "OpenGL ES ", 10) == 0 && ver[10] >= '3' && !strstr(e, "GL_EXT_unpack_subimage")) {
            char *c = malloc(strlen(e) + 32);
            if (!c)
                return s;
            sprintf(c, "%s%sGL_EXT_unpack_subimage", e, (*e && e[strlen(e) - 1] != ' ') ? " " : "");
            free(ext_cache);
            ext_cache = c;
            ext_src   = s;
            return (const GLubyte *) c;
        }
    }
    return s;
}
