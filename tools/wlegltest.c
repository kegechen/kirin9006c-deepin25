/* libmali 作为 Wayland 客户端：eglInitialize + 建窗口表面 + GLES 清屏交换若干帧 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static struct wl_compositor *comp;

static void on_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, "wl_compositor"))
        comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
}
static void on_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener lst = { on_global, on_remove };

int main(void)
{
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { perror("connect"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &lst, NULL);
    wl_display_roundtrip(dpy);

    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay ed = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, dpy, NULL);
    EGLint maj = 0, min = 0;
    if (!eglInitialize(ed, &maj, &min)) {
        printf("eglInitialize 失败 0x%x\n", eglGetError());
        return 2;
    }
    printf("EGL %d.%d vendor=%s\n", maj, min, eglQueryString(ed, EGL_VENDOR));
    EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(ed, ca, &cfg, 1, &n);
    printf("config 数=%d\n", n);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, cx);
    struct wl_surface *s = wl_compositor_create_surface(comp);
    struct wl_egl_window *w = wl_egl_window_create(s, 640, 480);
    EGLSurface es = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)w, NULL);
    printf("ctx=%p surface=%p err=0x%x\n", ctx, es, eglGetError());
    if (!eglMakeCurrent(ed, es, es, ctx)) { printf("makeCurrent 失败 0x%x\n", eglGetError()); return 3; }
    printf("GL %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    eglSwapInterval(ed, 0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 60; i++) {
        glClearColor(i / 60.0f, 0.3f, 0.6f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!eglSwapBuffers(ed, es)) { printf("swap 失败 0x%x (第 %d 帧)\n", eglGetError(), i); return 4; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("60 帧交换成功，%.1f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(ed, es);
    wl_egl_window_destroy(w);
    eglTerminate(ed);
    wl_display_disconnect(dpy);
    return 0;
}
