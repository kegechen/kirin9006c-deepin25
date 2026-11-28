/*
 * treeland 锁屏的指纹 / 人脸解锁（华为擎云 L540 / deepin 25）。
 *
 * 背景：treeland 锁屏把密码交给 ddm，ddm 带着密码一次性 pam_authenticate，conversation 立刻答出密码，
 *       deepin 的生物识别（org.deepin.dde.Authenticate1）在这条路上永远轮不到；treeland 0.11 也没有生物识别 UI。
 *       而解锁动作本身只是 ddm 调 logind UnlockSession，treeland 收到 logind 的 Unlock 信号就收起锁屏。
 * 做法：本服务（root）监听 logind Session 的 Lock / Unlock 信号：
 *   - 锁屏后用 Authenticate1.Authenticate(user, 指纹|人脸, Lock) 建认证 session；服务端只把该用户录入过的类型
 *     放进 session（看 FactorsInfo），没录入就什么都不做，录入后下次锁屏自动生效。指纹立即开始等待；
 *   - 人脸只在锁屏 2 秒后有用户动作（键盘 / 触控板 / 开盖 / 休眠唤醒）时才开，每次最多 15 秒，
 *     免得手动锁屏后人还坐在跟前就被立刻解开（dde-lock 里人脸成功后也要再点一下才解锁）；
 *   - 收到最终裁决 Status(-1, Success) 后调 logind UnlockSession；
 *   - 会话被密码解锁时结束认证 session、释放传感器和摄像头，并 ResetLimits（失败次数清零，被锁的指纹 / 人脸恢复）。
 * 锁屏界面：在 system bus 上以 org.hisi.BioUnlock1 提供状态（StateChanged / GetState），treeland 里的
 *   libtl-lockbio.so 据此显示指纹 / 人脸图标和提示，点人脸图标时调 StartFace。界面只显示，解锁仍在这里做。
 *   D-Bus 策略见 /etc/dbus-1/system.d/org.hisi.BioUnlock1.conf（root 持有名字，只有 greeter 用户 dde 能调用）。
 * 手机解锁：配对的手机经 hisi-phone-link（Wi-Fi / 蓝牙收发）发来指纹签名，验签在 hisi-bio-phone.c，
 *   通过后也走这里的 UnlockSession。配对命令 hisi-bio-unlock phone pair（hisi-bio-phonectl.c）。见 docs/phone-unlock.md。
 * 状态码、类型位、调用顺序按 dde-session-shell 的 DeepinAuthFramework / LockWorker。
 * 编译：gcc -O2 -Wall -o hisi-bio-unlock hisi-bio-unlock.c hisi-bio-phone.c hisi-bio-phonectl.c \
 *         $(pkg-config --cflags --libs gio-2.0 gio-unix-2.0 libcrypto)
 * 调试：hisi-bio-unlock -v   （打印全部 Status 信号）
 * 自测：加 -DHISI_BIO_SELFTEST 编译，HISI_BIO_TEST_PASSWORD=<密码> 运行，锁屏 3 秒后用密码因子走一遍解锁路径；
 *       测试构建还多一个 DebugSetState(指纹, 人脸) 方法，不录入也能让锁屏界面显示各种状态（-1 表示不覆盖）
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "hisi-bio.h"

#define AUTH_SVC        "org.deepin.dde.Authenticate1"
#define AUTH_PATH       "/org/deepin/dde/Authenticate1"
#define AUTH_IFACE      "org.deepin.dde.Authenticate1"
#define AUTH_SESS_IFACE "org.deepin.dde.Authenticate1.Session"
#define LOGIN_SVC       "org.freedesktop.login1"
#define LOGIN_PATH      "/org/freedesktop/login1"
#define LOGIN_MGR       "org.freedesktop.login1.Manager"
#define LOGIN_SESS      "org.freedesktop.login1.Session"

enum { AT_PASSWORD = 1 << 0, AT_FINGERPRINT = 1 << 1, AT_FACE = 1 << 2, AT_ALL = -1 };
#ifdef HISI_BIO_SELFTEST
#define AUTH_FLAGS (AT_FINGERPRINT | AT_FACE | AT_PASSWORD)
#else
#define AUTH_FLAGS (AT_FINGERPRINT | AT_FACE)
#endif
enum { APP_LOCK = 2 };
enum { AS_SUCCESS, AS_FAILURE, AS_CANCEL, AS_TIMEOUT, AS_ERROR, AS_VERIFY, AS_EXCEPTION,
       AS_PROMPT, AS_STARTED, AS_ENDED, AS_LOCKED, AS_RECOVER, AS_UNLOCKED };

#define LOCK_GRACE_US      (2 * G_USEC_PER_SEC)   /* 锁屏后这段时间内的输入不算“回来了”（Super+L 的抬键等） */
#define FACE_COOLDOWN_US   (3 * G_USEC_PER_SEC)   /* 人脸一轮结束后，隔这么久才允许再开 */
#define FACE_FAIL_HOLD_US  (10 * G_USEC_PER_SEC)  /* 人脸不匹配后多等一会儿，免得鼠标一动就连续失败把人脸锁掉 */
#define FACE_TIMEOUT_S     15
#define FP_BACKOFF_MAX_S   60
#define CALL_TIMEOUT_MS    20000
#define FP_RETRY_SHOW_MS   2000                   /* 界面上“指纹不匹配”保留这么久 */

/* 给锁屏界面的状态（StateChanged 的两个 int）。st.fp_ui / st.face_ui 为 0 表示默认（等待按压 / 空闲） */
enum { FP_OFF, FP_READY, FP_RETRY, FP_LOCKED, FP_PASSED };
enum { FACE_OFF, FACE_IDLE, FACE_SCANNING, FACE_FAILED, FACE_LOCKED, FACE_PASSED };

int verbose;

typedef struct {
    int fd;
    guint src;
} Input;

GDBusConnection *bus;
static GMainLoop *loop;

static struct {
    gboolean locked;
    char *sess_path, *sess_id, *user;
    gint64 lock_time;
    char *auth_path;          /* Authenticate1 认证 session；只能由创建它的这条连接调用 */
    int factors;              /* 该 session 实际可用的认证类型（没录入的类型服务端不会加进来） */
    guint status_sub;
    gboolean fp_running, face_running;
    guint fp_timer, face_timer, resume_timer;
    guint fp_backoff_s;
    gint64 face_next;         /* 早于这个时刻不开人脸 */
    GSList *inputs;
    int fp_ui, face_ui;       /* 界面状态，0 = 默认 */
    guint fp_ui_timer;
} st;

/* 最近一次发给界面的状态 */
static struct {
    char *user;
    int fp, face;
} ui;
/* 最近一次通知界面的手机连接状态（user 空 = 没有） */
static struct {
    char *user, *name;
} ui_ph;
static void ui_sync(void);
#ifdef HISI_BIO_SELFTEST
static int dbg_fp = -1, dbg_face = -1;
#endif

static void fp_start(void);
#ifdef HISI_BIO_SELFTEST
static gboolean selftest_cb(gpointer data);
#endif

static GVariant *call(const char *dest, const char *path, const char *iface, const char *method,
                      GVariant *args, const GVariantType *ret, GError **err)
{
    return g_dbus_connection_call_sync(bus, dest, path, iface, method, args, ret,
                                       G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, err);
}

/* 不关心返回值的调用；失败只记日志 */
static void call_quiet(const char *dest, const char *path, const char *iface, const char *method, GVariant *args)
{
    GError *e = NULL;
    GVariant *r = call(dest, path, iface, method, args, NULL, &e);
    if (r)
        g_variant_unref(r);
    else {
        DBG("%s.%s: %s", iface, method, e->message);
        g_error_free(e);
    }
}

/* 【锁屏界面状态】每个主循环入口处理完后调 ui_sync()，有变化才发 StateChanged */

static void ui_current(const char **user, int *fp, int *face)
{
    *user = "";
    *fp = FP_OFF;
    *face = FACE_OFF;
    if (!st.locked || !st.user)
        return;
    if (st.factors & AT_FINGERPRINT)
        *fp = st.fp_ui ? st.fp_ui : FP_READY;
    if (st.factors & AT_FACE)
        *face = st.face_ui ? st.face_ui : FACE_IDLE;
#ifdef HISI_BIO_SELFTEST
    if (dbg_fp >= 0)
        *fp = dbg_fp;
    if (dbg_face >= 0)
        *face = dbg_face;
#endif
    if (*fp || *face)
        *user = st.user;
}

/* 锁屏界面上的“手机已连接”：锁着、且该用户有手机连着电脑时才有。名字不放进广播信号（任何人都能听），
 * 信号只通知变了，锁屏界面再调 GetPhone 取（D-Bus 策略只放行 dde） */
static void ui_phone_sync(void)
{
    const char *user = "", *name;
    char *label = NULL;
    guint n = 0;

    if (st.locked && st.user && (name = phone_online_name(st.user, &n))) {
        user = st.user;
        label = n > 1 ? g_strdup_printf("%s 等 %u 台", name, n) : g_strdup(name);
    }
    if (!g_strcmp0(user, ui_ph.user) && !g_strcmp0(label ? label : "", ui_ph.name)) {
        g_free(label);
        return;
    }
    g_free(ui_ph.user);
    g_free(ui_ph.name);
    ui_ph.user = g_strdup(user);
    ui_ph.name = label ? label : g_strdup("");
    DBG("界面手机：用户=%s 手机=%s", user, ui_ph.name);
    g_dbus_connection_emit_signal(bus, NULL, UI_PATH, UI_IFACE, "PhoneChanged", NULL, NULL);
}

void bio_ui_refresh(void)
{
    ui_sync();
}

static void ui_sync(void)
{
    const char *user;
    int fp, face;

    ui_phone_sync();
    ui_current(&user, &fp, &face);
    if (!g_strcmp0(user, ui.user) && fp == ui.fp && face == ui.face)
        return;
    g_free(ui.user);
    ui.user = g_strdup(user);
    ui.fp = fp;
    ui.face = face;
    DBG("界面状态：用户=%s 指纹=%d 人脸=%d", user, fp, face);
    g_dbus_connection_emit_signal(bus, NULL, UI_PATH, UI_IFACE, "StateChanged",
                                  g_variant_new("(sii)", user, fp, face), NULL);
}

static gboolean fp_ui_timer_cb(gpointer data)
{
    st.fp_ui_timer = 0;
    if (st.fp_ui == FP_RETRY)
        st.fp_ui = 0;
    ui_sync();
    return G_SOURCE_REMOVE;
}

static void fp_ui_set(int state)
{
    if (st.fp_ui_timer) {
        g_source_remove(st.fp_ui_timer);
        st.fp_ui_timer = 0;
    }
    st.fp_ui = state;
    if (state == FP_RETRY)
        st.fp_ui_timer = g_timeout_add(FP_RETRY_SHOW_MS, fp_ui_timer_cb, NULL);
}

/* 【Authenticate1 认证 session】 */

static void auth_drop(gboolean polite)
{
    if (st.fp_timer) {
        g_source_remove(st.fp_timer);
        st.fp_timer = 0;
    }
    if (st.face_timer) {
        g_source_remove(st.face_timer);
        st.face_timer = 0;
    }
    if (st.status_sub) {
        g_dbus_connection_signal_unsubscribe(bus, st.status_sub);
        st.status_sub = 0;
    }
    if (st.auth_path && polite) {
        call_quiet(AUTH_SVC, st.auth_path, AUTH_SESS_IFACE, "End", g_variant_new("(i)", AT_ALL));
        call_quiet(AUTH_SVC, st.auth_path, AUTH_SESS_IFACE, "Quit", NULL);
    }
    g_clear_pointer(&st.auth_path, g_free);
    st.factors = 0;
    st.fp_running = st.face_running = FALSE;
    if (st.face_ui == FACE_SCANNING)
        st.face_ui = 0;
}

static void on_status(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *member, GVariant *params, gpointer data);

static GVariant *authenticate(GError **e)
{
    return call(AUTH_SVC, AUTH_PATH, AUTH_IFACE, "Authenticate",
                g_variant_new("(sii)", st.user, AUTH_FLAGS, APP_LOCK), G_VARIANT_TYPE("(s)"), e);
}

/* Session.FactorsInfo a(iiib)：(类型, 优先级, 输入方式, 是否必需)。服务端只把设备在、且该用户录入过的类型加进 session */
static int auth_factors(void)
{
    GError *e = NULL;
    GVariant *r, *v;
    GVariantIter it;
    gint32 type, prio, input;
    gboolean required;
    int mask = 0;

    r = call(AUTH_SVC, st.auth_path, "org.freedesktop.DBus.Properties", "Get",
             g_variant_new("(ss)", AUTH_SESS_IFACE, "FactorsInfo"), G_VARIANT_TYPE("(v)"), &e);
    if (!r) {
        WARN("读取 FactorsInfo 失败：%s", e->message);
        g_error_free(e);
        return AUTH_FLAGS;   /* 读不到就都试一下 */
    }
    g_variant_get(r, "(v)", &v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE("a(iiib)"))) {
        g_variant_iter_init(&it, v);
        while (g_variant_iter_next(&it, "(iiib)", &type, &prio, &input, &required))
            mask |= type;
    }
    g_variant_unref(v);
    g_variant_unref(r);
    return mask;
}

static gboolean auth_begin(void)
{
    GError *e = NULL;
    GVariant *r;

    if (st.auth_path)
        return TRUE;
    r = authenticate(&e);
    if (!r) {
        WARN("创建认证 session 失败：%s", e->message);
        g_error_free(e);
        return FALSE;
    }
    g_variant_get(r, "(s)", &st.auth_path);
    g_variant_unref(r);
    if (st.auth_path[0] != '/') {
        WARN("Authenticate 返回了无效路径：%s", st.auth_path);
        g_clear_pointer(&st.auth_path, g_free);
        return FALSE;
    }
    st.status_sub = g_dbus_connection_signal_subscribe(bus, AUTH_SVC, AUTH_SESS_IFACE, "Status", st.auth_path,
                                                       NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_status, NULL, NULL);
    st.factors = auth_factors();
    DBG("认证 session %s（用户 %s，可用类型 0x%x）", st.auth_path, st.user, st.factors);
    return TRUE;
}

/* 返回 TRUE 表示该类型已开始认证 */
static gboolean auth_start(int flag, int timeout)
{
    GError *e = NULL;
    GVariant *r;
    gint32 failnum;

    if (!auth_begin())
        return FALSE;
    r = call(AUTH_SVC, st.auth_path, AUTH_SESS_IFACE, "Start", g_variant_new("(ii)", flag, timeout),
             G_VARIANT_TYPE("(i)"), &e);
    if (!r) {
        /* session 失效（服务重启等）：丢掉，下次重建 */
        WARN("Start(%d) 失败：%s", flag, e->message);
        g_error_free(e);
        auth_drop(FALSE);
        return FALSE;
    }
    g_variant_get(r, "(i)", &failnum);
    g_variant_unref(r);
    DBG("Start(%d, %d) failNum=%d", flag, timeout, failnum);
    return failnum == 0;
}

static void auth_end(int flag)
{
    if (st.auth_path)
        call_quiet(AUTH_SVC, st.auth_path, AUTH_SESS_IFACE, "End", g_variant_new("(i)", flag));
}

/* 【指纹 / 人脸】 */

static gboolean fp_timer_cb(gpointer data)
{
    st.fp_timer = 0;
    fp_start();
    ui_sync();
    return G_SOURCE_REMOVE;
}

static void fp_schedule(guint ms)
{
    if (st.locked && !st.fp_timer)
        st.fp_timer = g_timeout_add(ms, fp_timer_cb, NULL);
}

static void fp_start(void)
{
    if (!st.locked || st.fp_running)
        return;
    if (auth_begin() && !(st.factors & AT_FINGERPRINT))
        return;   /* 没录指纹 */
    if (auth_start(AT_FINGERPRINT, -1)) {
        st.fp_running = TRUE;
        st.fp_backoff_s = 0;
        DBG("指纹等待中");
        return;
    }
    /* 没录指纹、被锁定、设备忙：退避重试（锁定解除会有 AS_Unlocked，届时立即重试） */
    st.fp_backoff_s = st.fp_backoff_s ? MIN(st.fp_backoff_s * 2, FP_BACKOFF_MAX_S) : 5;
    fp_schedule(st.fp_backoff_s * 1000);
}

static void face_stopped(gint64 hold_us)
{
    if (st.face_timer) {
        g_source_remove(st.face_timer);
        st.face_timer = 0;
    }
    st.face_running = FALSE;
    st.face_next = MAX(st.face_next, g_get_monotonic_time() + hold_us);
    if (st.face_ui == FACE_SCANNING)
        st.face_ui = 0;
}

/* 服务端超时（或没录人脸时）不一定发结束状态：到点还没结束就自己 End，否则这次锁屏人脸再也不会重开 */
static gboolean face_timer_cb(gpointer data)
{
    st.face_timer = 0;
    if (st.face_running) {
        DBG("人脸超时，结束本轮");
        auth_end(AT_FACE);
        face_stopped(FACE_COOLDOWN_US);
    }
    ui_sync();
    return G_SOURCE_REMOVE;
}

static void face_start(gboolean force)
{
    gint64 now = g_get_monotonic_time();

    if (!st.locked || st.face_running)
        return;
    if (!force && (now - st.lock_time < LOCK_GRACE_US || now < st.face_next))
        return;
    st.face_next = now + FACE_COOLDOWN_US;
    if (auth_begin() && !(st.factors & AT_FACE))
        return;   /* 没录人脸 */
    if (auth_start(AT_FACE, FACE_TIMEOUT_S)) {
        st.face_running = TRUE;
        st.face_ui = FACE_SCANNING;
        st.face_timer = g_timeout_add_seconds(FACE_TIMEOUT_S + 3, face_timer_cb, NULL);
        DBG("人脸识别中（最多 %d 秒）", FACE_TIMEOUT_S);
    } else {
        st.face_next = now + FACE_FAIL_HOLD_US;
    }
}

static gboolean do_unlock(const char *how);

static void on_status(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *member, GVariant *params, gpointer data)
{
    gint32 flag, code;
    const char *msg;

    if (!st.auth_path || g_strcmp0(path, st.auth_path))
        return;
    g_variant_get(params, "(ii&s)", &flag, &code, &msg);
    DBG("Status flag=%d code=%d msg=%s", flag, code, msg);

    if (flag == AT_ALL) {
        if (code == AS_SUCCESS) {
            ui_sync();   /* 先让界面显示“验证成功”，再解锁 */
            do_unlock("生物识别通过");
        }
        ui_sync();
        return;
    }
    if (flag == AT_FINGERPRINT) {
        switch (code) {
        case AS_STARTED:
        case AS_VERIFY:
        case AS_PROMPT:
            st.fp_running = TRUE;
            break;
        case AS_SUCCESS:
            INFO("指纹匹配");
            fp_ui_set(FP_PASSED);
            break;
        case AS_FAILURE:
            /* 同 dde-lock：失败后 End 再重开，直到被锁定 */
            INFO("指纹不匹配：%s", msg);
            auth_end(AT_FINGERPRINT);
            st.fp_running = FALSE;
            fp_ui_set(FP_RETRY);
            fp_schedule(500);
            break;
        case AS_LOCKED:
            INFO("指纹已锁定（需要密码解锁一次）：%s", msg);
            st.fp_running = FALSE;
            fp_ui_set(FP_LOCKED);
            break;
        case AS_UNLOCKED:
        case AS_RECOVER:
            st.fp_running = FALSE;
            st.fp_backoff_s = 0;
            if (st.fp_ui == FP_LOCKED)
                fp_ui_set(0);
            fp_schedule(500);
            break;
        case AS_TIMEOUT:
        case AS_ERROR:
        case AS_EXCEPTION:
            WARN("指纹认证异常（%d）：%s", code, msg);
            auth_end(AT_FINGERPRINT);
            st.fp_running = FALSE;
            st.fp_backoff_s = st.fp_backoff_s ? MIN(st.fp_backoff_s * 2, FP_BACKOFF_MAX_S) : 5;
            fp_schedule(st.fp_backoff_s * 1000);
            break;
        case AS_CANCEL:
        case AS_ENDED:
            st.fp_running = FALSE;
            fp_schedule(1000);
            break;
        }
    } else if (flag == AT_FACE) {
        switch (code) {
        case AS_SUCCESS:
            INFO("人脸匹配");
            st.face_ui = FACE_PASSED;
            break;
        case AS_FAILURE:
            INFO("人脸不匹配：%s", msg);
            auth_end(AT_FACE);
            face_stopped(FACE_FAIL_HOLD_US);
            st.face_ui = FACE_FAILED;
            break;
        case AS_LOCKED:
            INFO("人脸已锁定（需要密码解锁一次）：%s", msg);
            face_stopped(FACE_FAIL_HOLD_US);
            st.face_ui = FACE_LOCKED;
            break;
        case AS_UNLOCKED:
        case AS_RECOVER:
            if (st.face_ui == FACE_LOCKED)
                st.face_ui = 0;
            break;
        case AS_ERROR:
        case AS_EXCEPTION:
            WARN("人脸认证异常（%d）：%s", code, msg);
            /* fallthrough */
        case AS_TIMEOUT:
        case AS_CANCEL:
        case AS_ENDED:
            face_stopped(FACE_COOLDOWN_US);
            break;
        }
    }
    ui_sync();
}

/* 【输入动作：判断“人回来了”，再开人脸】 */

static void on_activity(void)
{
    if (!st.locked)
        return;
    face_start(FALSE);
    if (!st.fp_running && !st.fp_timer)
        fp_start();
}

static gboolean on_input(int fd, GIOCondition cond, gpointer data)
{
    Input *in = data;
    struct input_event ev[64];
    gboolean active = FALSE;

    for (;;) {
        ssize_t n = read(fd, ev, sizeof ev);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            break;
        if (n <= 0)
            goto gone;
        for (size_t i = 0; i < (size_t)n / sizeof *ev; i++) {
            switch (ev[i].type) {
            case EV_KEY:
                active |= ev[i].value == 1;
                break;
            case EV_REL:
            case EV_ABS:
                active = TRUE;
                break;
            case EV_SW:
                active |= ev[i].code == SW_LID && ev[i].value == 0;   /* 开盖；耳机 / HDMI 插拔不算 */
                break;
            }
        }
    }
    if (cond & (G_IO_HUP | G_IO_ERR))
        goto gone;
    if (active) {
        on_activity();
        ui_sync();
    }
    return G_SOURCE_CONTINUE;
gone:
    close(in->fd);
    in->fd = -1;
    in->src = 0;
    return G_SOURCE_REMOVE;
}

/* 只在锁屏期间打开输入设备（只看有没有动作，不记录按键内容） */
static void inputs_open(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *de;

    if (!d)
        return;
    while ((de = readdir(d))) {
        char p[300];
        unsigned long bits = 0;
        int fd;

        if (strncmp(de->d_name, "event", 5))
            continue;
        snprintf(p, sizeof p, "/dev/input/%s", de->d_name);
        fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (ioctl(fd, EVIOCGBIT(0, sizeof bits), &bits) < 0
            || !(bits & ((1UL << EV_KEY) | (1UL << EV_REL) | (1UL << EV_ABS) | (1UL << EV_SW)))) {
            close(fd);
            continue;
        }
        Input *in = g_new0(Input, 1);
        in->fd = fd;
        in->src = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_input, in);
        st.inputs = g_slist_prepend(st.inputs, in);
    }
    closedir(d);
}

static void input_free(gpointer data)
{
    Input *in = data;

    if (in->src)
        g_source_remove(in->src);
    if (in->fd >= 0)
        close(in->fd);
    g_free(in);
}

static void inputs_close(void)
{
    g_slist_free_full(st.inputs, input_free);
    st.inputs = NULL;
}

/* 【锁屏状态】 */

static void end_lock(gboolean unlocked)
{
    if (!st.locked)
        return;
    auth_drop(TRUE);
    inputs_close();
    if (st.resume_timer) {
        g_source_remove(st.resume_timer);
        st.resume_timer = 0;
    }
    fp_ui_set(0);
    st.face_ui = 0;
    /* 解锁说明本人已通过认证：同 dde-lock 密码解锁后的行为，清掉失败计数，被锁的指纹 / 人脸恢复 */
    if (unlocked)
        call_quiet(AUTH_SVC, AUTH_PATH, AUTH_IFACE, "ResetLimits", g_variant_new("(s)", st.user));
    g_clear_pointer(&st.sess_path, g_free);
    g_clear_pointer(&st.sess_id, g_free);
    g_clear_pointer(&st.user, g_free);
    st.locked = FALSE;
}

static gboolean do_unlock(const char *how)
{
    GError *e = NULL;
    GVariant *r;

    if (!st.locked)
        return FALSE;
    INFO("%s，解锁会话 %s（%s）", how, st.sess_id, st.user);
    r = call(LOGIN_SVC, LOGIN_PATH, LOGIN_MGR, "UnlockSession", g_variant_new("(s)", st.sess_id), NULL, &e);
    if (!r) {
        WARN("UnlockSession 失败：%s", e->message);
        g_error_free(e);
        /* 认证 session 已经给出成功裁决，不能再用；重建，否则本次锁屏里指纹再也不会启动 */
        auth_drop(FALSE);
        fp_ui_set(0);
        st.face_ui = 0;
        st.fp_backoff_s = 0;
        fp_schedule(2000);
        ui_sync();
        return FALSE;
    }
    g_variant_unref(r);
    end_lock(TRUE);
    return TRUE;
}

typedef struct {
    char *id, *name, *service, *klass, *state;
    gboolean remote, active;
} SessProps;

static void sess_clear(SessProps *s)
{
    g_free(s->id);
    g_free(s->name);
    g_free(s->service);
    g_free(s->klass);
    g_free(s->state);
    memset(s, 0, sizeof *s);
}

static gboolean session_props(const char *path, SessProps *s)
{
    GError *e = NULL;
    GVariant *r, *props;

    memset(s, 0, sizeof *s);
    r = call(LOGIN_SVC, path, "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", LOGIN_SESS),
             G_VARIANT_TYPE("(a{sv})"), &e);
    if (!r) {
        WARN("读取会话 %s 属性失败：%s", path, e->message);
        g_error_free(e);
        return FALSE;
    }
    props = g_variant_get_child_value(r, 0);
    g_variant_lookup(props, "Id", "s", &s->id);
    g_variant_lookup(props, "Name", "s", &s->name);
    g_variant_lookup(props, "Service", "s", &s->service);
    g_variant_lookup(props, "Class", "s", &s->klass);
    g_variant_lookup(props, "State", "s", &s->state);
    g_variant_lookup(props, "Remote", "b", &s->remote);
    g_variant_lookup(props, "Active", "b", &s->active);
    g_variant_unref(props);
    g_variant_unref(r);
    return s->id && s->name;
}

/* 只管 ddm 起的本地图形会话；dde 是 greeter 自己的用户 */
static gboolean local_graphical(const SessProps *s)
{
    return !g_strcmp0(s->klass, "user") && s->service && !strncmp(s->service, "ddm", 3)
        && g_strcmp0(s->name, "dde") && !s->remote;
}

static void handle_lock(const char *path)
{
    SessProps s;

    if (!session_props(path, &s))
        goto out;
    if (!local_graphical(&s)) {
        DBG("忽略会话 %s 的 Lock（class=%s service=%s user=%s）", s.id, s.klass, s.service, s.name);
        goto out;
    }
    if (st.locked && !g_strcmp0(st.sess_path, path)) {
        fp_start();
        goto out;
    }
    end_lock(FALSE);
    st.locked = TRUE;
    st.sess_path = g_strdup(path);
    st.sess_id = g_steal_pointer(&s.id);
    st.user = g_steal_pointer(&s.name);
    st.lock_time = g_get_monotonic_time();
    st.face_next = 0;
    st.fp_backoff_s = 0;
    if (!auth_begin()) {
        INFO("会话 %s（%s）已锁定，认证服务暂不可用，稍后重试", st.sess_id, st.user);
        inputs_open();
        fp_schedule(5000);
        goto out;
    }
    if (!(st.factors & (AT_FINGERPRINT | AT_FACE))) {
        /* 没录入：服务端 session 里没有任何生物识别项，什么都不会发生。下次锁屏会重新查，录入后自动生效 */
        INFO("会话 %s（%s）已锁定：该用户在 deepin 里还没有录入指纹或人脸，只能用密码", st.sess_id, st.user);
#ifndef HISI_BIO_SELFTEST
        goto out;
#endif
    } else {
        INFO("会话 %s（%s）已锁定：%s%s", st.sess_id, st.user,
             st.factors & AT_FINGERPRINT ? "指纹等待中；" : "",
             st.factors & AT_FACE ? "有键盘 / 触控板动作时开人脸" : "");
    }
    if (st.factors & AT_FACE)
        inputs_open();
    fp_start();
#ifdef HISI_BIO_SELFTEST
    g_timeout_add_seconds(3, selftest_cb, NULL);
#endif
out:
    sess_clear(&s);
}

/* 【给手机解锁用】 */

/* 启动时找当前已锁屏的本地图形会话：服务是 root，不能拿自己的用户名去查 */
static char *locked_session_path(void)
{
    GError *e = NULL;
    GVariant *r;
    GVariantIter *it;
    const char *sid, *sname, *seat, *path;
    guint32 uid;
    char *found = NULL;

    r = call(LOGIN_SVC, LOGIN_PATH, LOGIN_MGR, "ListSessions", NULL, G_VARIANT_TYPE("(a(susso))"), &e);
    if (!r) {
        WARN("ListSessions 失败：%s", e->message);
        g_error_free(e);
        return NULL;
    }
    g_variant_get(r, "(a(susso))", &it);
    while (g_variant_iter_loop(it, "(&su&s&s&o)", &sid, &uid, &sname, &seat, &path)) {
        SessProps s;
        GVariant *p;

        if (!session_props(path, &s) || !local_graphical(&s)) {
            sess_clear(&s);
            continue;
        }
        p = call(LOGIN_SVC, path, "org.freedesktop.DBus.Properties", "Get",
                 g_variant_new("(ss)", LOGIN_SESS, "LockedHint"), G_VARIANT_TYPE("(v)"), &e);
        if (p) {
            GVariant *v = g_variant_get_child_value(p, 0);
            GVariant *b = g_variant_get_variant(v);
            gboolean locked = FALSE;

            g_variant_get(b, "b", &locked);
            g_variant_unref(b);
            g_variant_unref(v);
            g_variant_unref(p);
            if (locked) {
                found = g_strdup(path);
                sess_clear(&s);
                break;
            }
        } else {
            WARN("读会话 %s 的 LockedHint 失败：%s", sid, e->message);
            g_clear_error(&e);
        }
        sess_clear(&s);
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    return found;
}

static char *user_session(const char *user)
{
    GError *e = NULL;
    GVariant *r;
    GVariantIter *it;
    const char *sid, *sname, *seat, *path;
    guint32 uid;
    char *best = NULL;
    gboolean best_active = FALSE;

    r = call(LOGIN_SVC, LOGIN_PATH, LOGIN_MGR, "ListSessions", NULL, G_VARIANT_TYPE("(a(susso))"), &e);
    if (!r) {
        WARN("ListSessions 失败：%s", e->message);
        g_error_free(e);
        return NULL;
    }
    g_variant_get(r, "(a(susso))", &it);
    while (g_variant_iter_loop(it, "(&su&s&s&o)", &sid, &uid, &sname, &seat, &path)) {
        SessProps s;

        if (g_strcmp0(sname, user) || !session_props(path, &s))
            continue;
        if (local_graphical(&s) && g_strcmp0(s.state, "closing") && (!best || (s.active && !best_active))) {
            g_free(best);
            best = g_steal_pointer(&s.id);
            best_active = s.active;
        }
        sess_clear(&s);
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    return best;
}

void bio_session_info(const char *user, gboolean *has_session, gboolean *locked, gint64 *locked_for_s)
{
    *locked = st.locked && !g_strcmp0(st.user, user);
    *locked_for_s = *locked ? (g_get_monotonic_time() - st.lock_time) / G_USEC_PER_SEC : 0;
    if (*locked) {
        *has_session = TRUE;
    } else {
        char *id = user_session(user);
        *has_session = id != NULL;
        g_free(id);
    }
}

int bio_unlock_user(const char *user, const char *how)
{
    GError *e = NULL;
    GVariant *r;
    char *id;

    if (st.locked && !g_strcmp0(st.user, user)) {
        gboolean ok = do_unlock(how);
        ui_sync();
        return ok ? PS_OK : PS_ERROR;
    }
    /* 没收到过 Lock 信号（例如 treeland 重启后直接出的锁屏）：照样对该用户的会话发 UnlockSession，
     * treeland 收到 Unlock 就收起锁屏；本来没锁也无害 */
    id = user_session(user);
    if (!id)
        return PS_NO_SESSION;
    INFO("%s，解锁会话 %s（%s；之前没收到锁屏信号）", how, id, user);
    r = call(LOGIN_SVC, LOGIN_PATH, LOGIN_MGR, "UnlockSession", g_variant_new("(s)", id), NULL, &e);
    g_free(id);
    if (!r) {
        WARN("UnlockSession 失败：%s", e->message);
        g_error_free(e);
        return PS_ERROR;
    }
    g_variant_unref(r);
    return PS_OK;
}

int bio_lock_user(const char *user, const char *how)
{
    GError *e = NULL;
    GVariant *r;
    char *id;

    /* st.locked 可能漏过 Unlock 信号而过时，所以锁着也照样发一次 LockSession（treeland 已锁时是空操作）。
     * logind 的 LockedHint 在这里不可靠（treeland 不设），不拿它判断 */
    id = user_session(user);
    if (!id)
        return PS_NO_SESSION;
    INFO("%s，锁定会话 %s（%s）%s", how, id, user, st.locked && !g_strcmp0(st.user, user) ? "（本来就锁着）" : "");
    r = call(LOGIN_SVC, LOGIN_PATH, LOGIN_MGR, "LockSession", g_variant_new("(s)", id), NULL, &e);
    g_free(id);
    if (!r) {
        WARN("LockSession 失败：%s", e->message);
        g_error_free(e);
        return PS_ERROR;
    }
    g_variant_unref(r);
    return PS_OK;
}

#ifdef HISI_BIO_SELFTEST
/* 仅测试构建（-DHISI_BIO_SELFTEST）：不录生物特征也能验证“认证通过 → UnlockSession”这条路。
 * 锁屏 3 秒后用环境变量 HISI_BIO_TEST_PASSWORD 走一次密码因子；正式构建里没有这段 */
static gboolean selftest_cb(gpointer data)
{
    const char *pw = g_getenv("HISI_BIO_TEST_PASSWORD");

    if (!st.locked || !pw || !auth_start(AT_PASSWORD, -1))
        return G_SOURCE_REMOVE;
    INFO("自测：提交密码因子");
    call_quiet(AUTH_SVC, st.auth_path, AUTH_SESS_IFACE, "SetToken",
               g_variant_new("(i@ay)", AT_PASSWORD,
                             g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pw, strlen(pw), 1)));
    return G_SOURCE_REMOVE;
}
#endif

static void on_session_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                              const char *member, GVariant *params, gpointer data)
{
    if (!strcmp(member, "Lock")) {
        handle_lock(path);
    } else if (!strcmp(member, "Unlock") && st.locked && !g_strcmp0(path, st.sess_path)) {
        INFO("会话 %s 已解锁，释放指纹 / 摄像头", st.sess_id);
        end_lock(TRUE);
    }
    ui_sync();
}

static gboolean resume_cb(gpointer data)
{
    st.resume_timer = 0;
    st.fp_backoff_s = 0;
    fp_start();
    face_start(TRUE);   /* 开盖 / 按电源键唤醒，就是要用了 */
    ui_sync();
    return G_SOURCE_REMOVE;
}

static void on_sleep(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                     const char *member, GVariant *params, gpointer data)
{
    gboolean going;

    g_variant_get(params, "(b)", &going);
    if (!st.locked)
        return;
    if (going) {
        DBG("即将休眠，先释放生物识别设备");
        auth_drop(TRUE);
        if (st.resume_timer) {
            g_source_remove(st.resume_timer);
            st.resume_timer = 0;
        }
    } else if (!st.resume_timer) {
        st.resume_timer = g_timeout_add(1500, resume_cb, NULL);
    }
    ui_sync();
}

static void on_auth_owner(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                          const char *member, GVariant *params, gpointer data)
{
    /* 认证服务重启：旧 session 作废 */
    if (!st.auth_path)
        return;
    WARN("认证服务重启，重建认证 session");
    auth_drop(FALSE);
    st.fp_backoff_s = 0;
    fp_schedule(2000);
    ui_sync();
}

/* 【锁屏界面接口 org.hisi.BioUnlock1】 */

static const char ui_xml[] =
    "<node>"
    " <interface name='" UI_IFACE "'>"
    "  <method name='GetState'>"
    "   <arg type='s' name='user' direction='out'/>"
    "   <arg type='i' name='fingerprint' direction='out'/>"
    "   <arg type='i' name='face' direction='out'/>"
    "  </method>"
    "  <method name='StartFace'/>"
    "  <method name='GetPhone'>"
    "   <arg type='s' name='user' direction='out'/>"
    "   <arg type='s' name='name' direction='out'/>"
    "  </method>"
#ifdef HISI_BIO_SELFTEST
    "  <method name='DebugSetState'>"
    "   <arg type='i' name='fingerprint' direction='in'/>"
    "   <arg type='i' name='face' direction='in'/>"
    "  </method>"
#endif
    "  <signal name='StateChanged'>"
    "   <arg type='s' name='user'/>"
    "   <arg type='i' name='fingerprint'/>"
    "   <arg type='i' name='face'/>"
    "  </signal>"
    "  <signal name='PhoneChanged'/>"
    " </interface>"
    "</node>";

static void ui_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    if (!strcmp(method, "GetState")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(sii)", ui.user, ui.fp, ui.face));
        return;
    }
    if (!strcmp(method, "GetPhone")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ss)", ui_ph.user, ui_ph.name));
        return;
    }
    if (!strcmp(method, "StartFace")) {
        g_dbus_method_invocation_return_value(inv, NULL);
        if (st.locked && st.face_ui != FACE_LOCKED) {
            DBG("锁屏界面请求开人脸");
            face_start(TRUE);
            ui_sync();
        }
        return;
    }
#ifdef HISI_BIO_SELFTEST
    if (!strcmp(method, "DebugSetState")) {
        g_variant_get(params, "(ii)", &dbg_fp, &dbg_face);
        g_dbus_method_invocation_return_value(inv, NULL);
        ui_sync();
        return;
    }
#endif
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "没有方法 %s", method);
}

static void ui_name_lost(GDBusConnection *c, const char *name, gpointer data)
{
    WARN("拿不到 D-Bus 名字 %s（/etc/dbus-1/system.d/%s.conf 没装？），锁屏界面看不到指纹 / 人脸状态，解锁照常",
         name, name);
}

static void ui_export(void)
{
    static const GDBusInterfaceVTable vtable = { ui_method, NULL, NULL };
    GError *e = NULL;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(ui_xml, &e);

    ui.user = g_strdup("");
    ui_ph.user = g_strdup("");
    ui_ph.name = g_strdup("");
    if (!node || !g_dbus_connection_register_object(bus, UI_PATH, node->interfaces[0], &vtable, NULL, NULL, &e)) {
        WARN("导出锁屏界面接口失败：%s", e->message);
        g_error_free(e);
    } else {
        g_bus_own_name_on_connection(bus, UI_SVC, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, ui_name_lost, NULL, NULL);
    }
    if (node)
        g_dbus_node_info_unref(node);
}

static gboolean on_term(gpointer data)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    GError *e = NULL;

    if (argc > 1 && !strcmp(argv[1], "phone"))
        return phonectl_main(argc - 1, argv + 1);
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
    if (!bus) {
        LOG(3, "连不上 system bus：%s", e->message);
        return 1;
    }
    g_dbus_connection_signal_subscribe(bus, LOGIN_SVC, LOGIN_SESS, NULL, NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_session_signal, NULL, NULL);
    g_dbus_connection_signal_subscribe(bus, LOGIN_SVC, LOGIN_MGR, "PrepareForSleep", LOGIN_PATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_sleep, NULL, NULL);
    g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                                       "/org/freedesktop/DBus", AUTH_SVC, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_auth_owner, NULL, NULL);
    phone_init();   /* 先挂好手机解锁接口，ui_export 里才申请名字 */
    ui_export();
    /* 服务在锁屏期间重启（崩溃拉起、--fix 恢复、手动 restart）时收不到之前那次 Lock，
     * 主动问一次 logind，别让这次锁屏只剩密码、手机端也看不到截图 */
    {
        char *path = locked_session_path();

        if (path) {
            INFO("启动时会话 %s 已锁屏，接上", path);
            handle_lock(path);
            g_free(path);
        }
    }
    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_term, NULL);
    g_unix_signal_add(SIGINT, on_term, NULL);
    INFO("已启动，等待锁屏");
    g_main_loop_run(loop);
    end_lock(FALSE);
    return 0;
}
