/*
 * hisi-phone-link：手机指纹解锁的收发（用户 hisi-phone，无特权，见 hisi-phone-link.service）。
 *   Wi-Fi：TCP 47390，一条连接一个会话；UDP 47390 回应手机的发现广播。
 *   蓝牙：BLE GATT 外设（经 BlueZ D-Bus 注册），手机写 RX、电脑用 TX 通知回，内容和 TCP 一样是文本行。
 * 这里只拆文本行、校验 Base64 和长度，再经 D-Bus 调 hisi-bio-unlock 的 org.hisi.BioUnlock1.Phone；
 * 配对码、共享密钥、验签、截图都在那边，本进程碰不到。协议见 docs/phone-unlock.md。
 * 编译：gcc -O2 -Wall -o hisi-phone-link hisi-phone-link.c $(pkg-config --cflags --libs gio-2.0 gio-unix-2.0)
 * 调试：hisi-phone-link -v
 */
#define _GNU_SOURCE
#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <string.h>
#include "hisi-bio.h"

#define LINK_PORT     47390
#define MAX_TCP       4
/** TCP 会话用命令推进空闲计时，别让攻击者靠空 PING 一直占着名额；同时给会话一个绝对寿命上限。 */
#define IDLE_S        90
#define TCP_MAX_LIFE_S 900
#define IN_MAX        4096                 /* 手机发来的一行最长 */
#define BLE_BUSY_S    30                   /* 蓝牙会话这么久没动静，别的手机可以抢 */
#define BIO_TIMEOUT   40000
#define ADV_DURATION_S 60                  /* 蓝牙广播每次的时长（见 adv_renew 前的说明），不能超过 65 */
#define ADV_SLACK_MS   300                 /* 控制器上的广播到期后再等这么久才重新注册 */
#define ADV_RETRY_MS   1000                /* 不知道旧广播何时到期时，注册失败的重试间隔 */
#define ADV_RETRY_MAX  70                  /* 这样连续失败（已超过一整个时长）就放慢 */
#define ADV_SLOW_S     30
#define GONE_DELAY_MS  2000                /* 离开自动锁屏：设备断开后等这么久，确认不是电脑自己关了蓝牙 */

#define BLE_APP  "/org/hisi/phonelink"
#define BLE_SVC  BLE_APP "/service0"
#define BLE_RX   BLE_SVC "/rx"
#define BLE_TX   BLE_SVC "/tx"
#define BLE_ADV  BLE_APP "/adv"
#define UUID_SVC "5a3f0001-4c6e-4f1b-9d2a-7e8b1c0d4f60"
#define UUID_RX  "5a3f0002-4c6e-4f1b-9d2a-7e8b1c0d4f60"
#define UUID_TX  "5a3f0003-4c6e-4f1b-9d2a-7e8b1c0d4f60"
#define CHRC_IFACE "org.bluez.GattCharacteristic1"

int verbose;
GDBusConnection *bus;
static GMainLoop *loop;
static char pc_id[33];
static gboolean bio_up;

/* 【会话】TCP 连接或 BLE 连接各算一个 */

enum { KIND_TCP, KIND_BLE };
enum { OP_PAIR, OP_HELLO, OP_SHOT, OP_UNLOCK, OP_LOCK, OP_AWAY };

typedef struct {
    int refs;
    int kind;
    gboolean closed, closing, busy;
    gboolean eof;              /* TCP 对端已半关闭：处理完手头的请求、发完回复再断 */
    char *peer;               /* 日志用：IP 或蓝牙设备 */
    GString *in;
    char phone_id[33];
    GBytes *cn;                /* HELLO 成功后的挑战 */
    guint req_grace;           /* 在途的 AWAY 请求的秒数 */
    guint idle;
    guint life;
    gint64 last;
    /* TCP */
    GSocketConnection *conn;
    GCancellable *cancel;
    guint8 rbuf[2048];
    GQueue outq;
    gboolean writing;
    /* BLE */
    char *dev;
    guint16 mtu;
} Session;

static int n_tcp;
static Session *ble_sess;

/* 【离开自动锁屏】发过 AWAY（非 0）的蓝牙设备 → 哪台手机。设备断开（BlueZ Device1.Connected 变 false）后
 * 再等 GONE_DELAY_MS：这期间适配器断电 / BlueZ 退出说明是电脑自己关了蓝牙（BlueZ 先报设备断开、后报断电），
 * 不算离开；否则告诉 hisi-bio-unlock（Gone），由它按手机设的秒数倒计时锁屏。断开一次就用掉，手机连回来
 * 重新发 AWAY。表跟着设备走、不跟会话：蓝牙会话空闲超时关掉时设备可能还连着 */
typedef struct {
    char *dev;
    char phone_id[33];
    guint timer;
} Away;

static GHashTable *aways;      /* BlueZ 设备路径（Away.dev）→ Away* */

static void away_free(gpointer data)
{
    Away *a = data;

    if (a->timer)
        g_source_remove(a->timer);
    g_free(a->dev);
    g_free(a);
}

static void away_set(const char *dev, const char *phone_id, guint grace)
{
    Away *a;

    if (!grace) {
        g_hash_table_remove(aways, dev);
        return;
    }
    a = g_new0(Away, 1);
    a->dev = g_strdup(dev);
    g_strlcpy(a->phone_id, phone_id, sizeof a->phone_id);
    g_hash_table_replace(aways, a->dev, a);   /* 换掉旧的，连同可能在等的断开确认 */
}

static void gone_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(bus, res, &e);

    if (r) {
        g_variant_unref(r);
    } else {
        WARN("报告手机离开失败：%s", e->message);
        g_error_free(e);
    }
}

static gboolean away_fire(gpointer data)
{
    Away *a = data;

    a->timer = 0;
    INFO("[%s] 开着离开自动锁屏的手机 %.8s 蓝牙断开了", a->dev, a->phone_id);
    g_dbus_connection_call(bus, UI_SVC, UI_PATH, PHONE_IFACE, "Gone", g_variant_new("(s)", a->phone_id),
                           G_VARIANT_TYPE("(i)"), G_DBUS_CALL_FLAGS_NONE, BIO_TIMEOUT, NULL, gone_done, NULL);
    g_hash_table_remove(aways, a->dev);
    return G_SOURCE_REMOVE;
}

static void away_disconnected(const char *dev)
{
    Away *a = g_hash_table_lookup(aways, dev);

    if (a && !a->timer)
        a->timer = g_timeout_add(GONE_DELAY_MS, away_fire, a);
}

/* 电脑自己关了蓝牙：不算离开，全部作废 */
static void away_clear(const char *why)
{
    if (g_hash_table_size(aways))
        INFO("%s，离开自动锁屏暂停（手机连回来会重新开启）", why);
    g_hash_table_remove_all(aways);
}

static void session_pump(Session *s);
static void tcp_write_next(Session *s);
static void ble_send(Session *s, const char *data, gsize len);
static void adv_session_gone(void);
static void adv_lost(void);

static Session *session_ref(Session *s)
{
    s->refs++;
    return s;
}

static void session_unref(Session *s)
{
    if (--s->refs)
        return;
    g_string_free(s->in, TRUE);
    if (s->cn)
        g_bytes_unref(s->cn);
    g_queue_clear_full(&s->outq, (GDestroyNotify)g_bytes_unref);
    g_clear_object(&s->conn);
    g_clear_object(&s->cancel);
    g_free(s->peer);
    g_free(s->dev);
    g_free(s);
}

static void session_close(Session *s)
{
    if (s->closed)
        return;
    s->closed = TRUE;
    DBG("[%s] 断开", s->peer);
    if (s->idle) {
        g_source_remove(s->idle);
        s->idle = 0;
    }
    if (s->life) {
        g_source_remove(s->life);
        s->life = 0;
    }
    if (s->kind == KIND_TCP) {
        g_cancellable_cancel(s->cancel);
        g_io_stream_close(G_IO_STREAM(s->conn), NULL, NULL);
        n_tcp--;
    }
    if (s == ble_sess) {
        ble_sess = NULL;
        adv_session_gone();
    }
    session_unref(s);
}

/* 发完已排队的再断 */
static void session_end(Session *s)
{
    if (s->kind == KIND_TCP && (s->writing || !g_queue_is_empty(&s->outq)))
        s->closing = TRUE;
    else
        session_close(s);
}

static gboolean idle_cb(gpointer data)
{
    Session *s = data;

    s->idle = 0;
    DBG("[%s] 空闲超时", s->peer);
    session_close(s);
    return G_SOURCE_REMOVE;
}

static void session_touch(Session *s)
{
    s->last = g_get_monotonic_time();
    if (s->idle)
        g_source_remove(s->idle);
    s->idle = g_timeout_add_seconds(IDLE_S, idle_cb, s);
}

static gboolean tcp_life_cb(gpointer data)
{
    Session *s = data;

    s->life = 0;
    DBG("[%s] 连接达到最长寿命，断开", s->peer);
    session_close(s);
    return G_SOURCE_REMOVE;
}

static Session *session_new(int kind, const char *peer)
{
    Session *s = g_new0(Session, 1);

    s->refs = 1;
    s->kind = kind;
    s->peer = g_strdup(peer);
    s->in = g_string_new(NULL);
    g_queue_init(&s->outq);
    session_touch(s);
    /* 绝对寿命：哪怕一直发 PING 刷空闲计时，也不会永久占着 TCP 名额 */
    if (kind == KIND_TCP)
        s->life = g_timeout_add_seconds(TCP_MAX_LIFE_S, tcp_life_cb, s);
    return s;
}

static void send_line(Session *s, const char *line)
{
    gsize len = strlen(line);

    if (s->closed)
        return;
    if (s->kind == KIND_BLE) {
        char *l = g_strconcat(line, "\n", NULL);
        ble_send(s, l, len + 1);
        g_free(l);
        return;
    }
    g_queue_push_tail(&s->outq, g_bytes_new_take(g_strconcat(line, "\n", NULL), len + 1));
    tcp_write_next(s);
}

static const char *err_text(int code)
{
    switch (code) {
    case PS_UNKNOWN_PHONE: return "这台手机没有配对，或已在电脑上删除，请重新配对";
    case PS_NO_SESSION:    return "电脑上该用户没有登录";
    case PS_BAD_SIGNATURE: return "签名校验不通过";
    case PS_BAD_CHALLENGE: return "请求已过期，请重试";
    case PS_LOCKED_OUT:    return "失败次数太多，请 5 分钟后再试";
    case PS_PAIR_CLOSED:   return "电脑上没有在配对（二维码已过期），请在电脑上重新运行 sudo hisi-phone pair";
    case PS_BAD_CODE:      return "配对校验失败，请重新扫描电脑上的二维码";
    case PS_NOT_LOCKED:    return "电脑没有锁屏";
    case PS_BUSY:          return "电脑正忙（可能另一台手机正在连接），请稍后再试";
    case PS_BAD_REQUEST:   return "请求格式不对";
    case PS_UNAVAILABLE:   return "电脑上的解锁服务没有运行";
    default:               return "电脑内部错误";
    }
}

static void send_err(Session *s, int code)
{
    const char *t = err_text(code);
    char *b = g_base64_encode((const guchar *)t, strlen(t));
    char *line = g_strdup_printf("ERR %d %s", code, b);

    send_line(s, line);
    g_free(line);
    g_free(b);
}

static char *b64(GVariant *v)
{
    gsize n;
    const guint8 *d = g_variant_get_fixed_array(v, &n, 1);

    return g_base64_encode(d, n);
}

/* 严格的标准 Base64：字符集、长度对得上才解；长度超过 max 拒收 */
static GVariant *b64_ay(const char *s, gsize max)
{
    gsize len = strlen(s), n;
    guchar *d;

    if (!len || len % 4 || len / 4 * 3 > max + 2)
        return NULL;
    for (gsize i = 0; i < len; i++) {
        char c = s[i];
        if (!g_ascii_isalnum(c) && c != '+' && c != '/' && !(c == '=' && i >= len - 2))
            return NULL;
    }
    if (s[len - 2] == '=' && s[len - 1] != '=')
        return NULL;
    d = g_base64_decode(s, &n);
    if (n > max) {
        g_free(d);
        return NULL;
    }
    return g_variant_new_from_data(G_VARIANT_TYPE_BYTESTRING, d, n, TRUE, g_free, d);
}

static gboolean valid_id(const char *s)
{
    if (strlen(s) != 32)
        return FALSE;
    for (; *s; s++)
        if (!g_ascii_isdigit(*s) && !(*s >= 'a' && *s <= 'f'))
            return FALSE;
    return TRUE;
}

/* 【转给 hisi-bio-unlock】 */

typedef struct {
    Session *s;
    int op;
} Pending;

static void bio_reply(GObject *src, GAsyncResult *res, gpointer data)
{
    Pending *p = data;
    Session *s = p->s;
    int op = p->op, status = PS_ERROR;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(bus, res, &e);
    GVariant *a = NULL, *b = NULL;
    char *line = NULL;

    g_free(p);
    if (!r) {
        WARN("[%s] 调 hisi-bio-unlock 失败：%s", s->peer, e->message);
        status = g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
                 || g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)
                 || g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE)
                 || g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ? PS_UNAVAILABLE : PS_ERROR;
        g_error_free(e);
    } else if (op == OP_PAIR || op == OP_SHOT) {
        g_variant_get(r, "(i@ay)", &status, &a);
        if (!status) {
            char *x = b64(a);
            line = g_strdup_printf(op == OP_PAIR ? "PAIROK %s" : "SHOTOK %s", x);
            g_free(x);
        }
    } else if (op == OP_HELLO) {
        g_variant_get(r, "(i@ay@ay)", &status, &a, &b);
        if (!status) {
            char *x = b64(a), *y = b64(b);
            gsize n;
            const guint8 *cn = g_variant_get_fixed_array(a, &n, 1);

            if (s->cn)
                g_bytes_unref(s->cn);
            s->cn = g_bytes_new(cn, n);
            line = g_strdup_printf("CHAL %s %s", x, y);
            g_free(x);
            g_free(y);
        }
    } else if (op == OP_UNLOCK || op == OP_LOCK || op == OP_AWAY) {
        g_variant_get(r, "(i)", &status);
        if (!status)
            line = g_strdup("OK");
    }
    if ((op == OP_UNLOCK || op == OP_LOCK) && s->cn) {   /* 挑战用过一次就作废 */
        g_bytes_unref(s->cn);
        s->cn = NULL;
    }
    if (op == OP_AWAY && status == PS_OK && s->kind == KIND_BLE && s->dev)
        away_set(s->dev, s->phone_id, s->req_grace);
    if (op == OP_SHOT)
        DBG("[%s] 截图 → %d", s->peer, status);
    else if (op == OP_AWAY)
        INFO("[%s] 离开自动锁屏 %u 秒 %.8s → %d", s->peer, s->req_grace, s->phone_id, status);
    else
        INFO("[%s] %s %.8s → %d", s->peer,
             op == OP_PAIR ? "配对" : op == OP_HELLO ? "HELLO" : op == OP_UNLOCK ? "解锁" : "锁屏", s->phone_id, status);
    if (line)
        send_line(s, line);
    else
        send_err(s, status);
    g_free(line);
    if (a)
        g_variant_unref(a);
    if (b)
        g_variant_unref(b);
    if (r)
        g_variant_unref(r);
    s->busy = FALSE;
    session_pump(s);
    if (s->eof && !s->busy && !s->closed)
        session_end(s);
    session_unref(s);
}

static void call_bio(Session *s, int op, const char *method, GVariant *args, const char *ret)
{
    Pending *p = g_new0(Pending, 1);

    p->s = session_ref(s);
    p->op = op;
    s->busy = TRUE;
    g_dbus_connection_call(bus, UI_SVC, UI_PATH, PHONE_IFACE, method, args, G_VARIANT_TYPE(ret),
                           G_DBUS_CALL_FLAGS_NONE, BIO_TIMEOUT, NULL, bio_reply, p);
}

/* 十进制、无前导零、最多 4 位；zero_ok 时单独一个 "0" 也行 */
static gboolean parse_dec(const char *s, gboolean zero_ok, guint *w)
{
    if (!*s || strlen(s) > 4 || (*s == '0' && (s[1] || !zero_ok)))
        return FALSE;
    for (const char *c = s; *c; c++)
        if (!g_ascii_isdigit(*c))
            return FALSE;
    *w = (guint)g_ascii_strtoull(s, NULL, 10);
    return TRUE;
}

static void handle_line(Session *s, char *line)
{
    char **f = g_strsplit(line, " ", 8);
    guint n = g_strv_length(f);
    GVariant *a = NULL, *b = NULL;

    if (n == 1 && !strcmp(f[0], "PING")) {
        char *r = g_strdup_printf("PONG %s", pc_id);
        send_line(s, r);
        g_free(r);
    } else if (!bio_up) {
        send_err(s, PS_UNAVAILABLE);
    } else if (n == 6 && !strcmp(f[0], "PAIR") && !strcmp(f[1], "1") && valid_id(f[2])) {
        GVariant *name = b64_ay(f[3], 64);
        gsize len = 0;
        const char *nm = name ? g_variant_get_fixed_array(name, &len, 1) : NULL;
        char *str = nm ? g_strndup(nm, len) : NULL;
        gboolean ok = str && len && strlen(str) == len && g_utf8_validate(str, len, NULL);

        for (const char *c = str; ok && *c; c++)
            ok = (guchar)*c >= 0x20;
        a = b64_ay(f[4], 512);
        b = b64_ay(f[5], 32);
        if (ok && a && b) {
            g_strlcpy(s->phone_id, f[2], sizeof s->phone_id);
            call_bio(s, OP_PAIR, "Pair", g_variant_new("(ss@ay@ay)", f[2], str, a, b), "(iay)");
            a = b = NULL;
        } else {
            send_err(s, PS_BAD_REQUEST);
        }
        g_free(str);
        if (name)
            g_variant_unref(g_variant_ref_sink(name));
    } else if (n == 4 && !strcmp(f[0], "HELLO") && !strcmp(f[1], "1") && valid_id(f[2])
               && (a = b64_ay(f[3], 16))) {
        g_strlcpy(s->phone_id, f[2], sizeof s->phone_id);
        /* 只有认得出的手机才允许刷空闲计时；PING 之类的匿名请求不能靠心跳占住 TCP 名额 */
        if (s->kind == KIND_TCP && s->phone_id[0])
            session_touch(s);
        if (s->cn) {
            g_bytes_unref(s->cn);
            s->cn = NULL;
        }
        call_bio(s, OP_HELLO, "Hello", g_variant_new("(s@ay)", f[2], a), "(iayay)");
        a = NULL;
    } else if (n == 3 && !strcmp(f[0], "SHOT") && (b = b64_ay(f[2], 32))) {
        guint w;

        if (!s->cn)
            send_err(s, PS_BAD_CHALLENGE);
        else if (!parse_dec(f[1], FALSE, &w))
            send_err(s, PS_BAD_REQUEST);
        else {
            GVariant *cn = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, s->cn, TRUE);
            call_bio(s, OP_SHOT, "Shot", g_variant_new("(s@ayu@ay)", s->phone_id, cn, w, b), "(iay)");
            b = NULL;
        }
    } else if (n == 2 && !strcmp(f[0], "UNLOCK") && (b = b64_ay(f[1], 160))) {
        if (!s->cn) {
            send_err(s, PS_BAD_CHALLENGE);
        } else {
            GVariant *cn = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, s->cn, TRUE);
            call_bio(s, OP_UNLOCK, "Unlock", g_variant_new("(s@ay@ay)", s->phone_id, cn, b), "(i)");
            b = NULL;
        }
    } else if (n == 2 && !strcmp(f[0], "LOCK") && (b = b64_ay(f[1], 32))) {
        if (!s->cn) {
            send_err(s, PS_BAD_CHALLENGE);
        } else {
            GVariant *cn = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, s->cn, TRUE);
            call_bio(s, OP_LOCK, "Lock", g_variant_new("(s@ay@ay)", s->phone_id, cn, b), "(i)");
            b = NULL;
        }
    } else if (n == 3 && !strcmp(f[0], "AWAY") && (b = b64_ay(f[2], 32))) {
        guint g;

        /* 只认蓝牙链路的断开，Wi-Fi 上开没有意义 */
        if (s->kind != KIND_BLE || !parse_dec(f[1], TRUE, &g)) {
            send_err(s, PS_BAD_REQUEST);
        } else if (!s->cn) {
            send_err(s, PS_BAD_CHALLENGE);
        } else {
            GVariant *cn = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, s->cn, TRUE);
            s->req_grace = g;
            call_bio(s, OP_AWAY, "Away", g_variant_new("(s@ayu@ay)", s->phone_id, cn, g, b), "(i)");
            b = NULL;
        }
    } else {
        DBG("[%s] 看不懂的请求：%.40s", s->peer, line);
        send_err(s, PS_BAD_REQUEST);
    }
    if (a)
        g_variant_unref(g_variant_ref_sink(a));
    if (b)
        g_variant_unref(g_variant_ref_sink(b));
    g_strfreev(f);
}

/* 一次处理一行；在等 hisi-bio-unlock 回复时后面的先攒着 */
static void session_pump(Session *s)
{
    while (!s->closed && !s->busy) {
        char *nl = memchr(s->in->str, '\n', s->in->len), *line;

        if (!nl) {
            if (s->in->len > IN_MAX) {
                send_err(s, PS_BAD_REQUEST);
                session_end(s);
            }
            return;
        }
        line = g_strndup(s->in->str, nl - s->in->str);
        g_string_erase(s->in, 0, nl - s->in->str + 1);
        if (*line && line[strlen(line) - 1] == '\r')
            line[strlen(line) - 1] = 0;
        if (strlen(line) > IN_MAX || !g_utf8_validate(line, -1, NULL)) {
            send_err(s, PS_BAD_REQUEST);
            session_end(s);
        } else if (*line) {
            handle_line(s, line);
        }
        g_free(line);
    }
}

static void session_feed(Session *s, const void *data, gsize len)
{
    if (s->closed)
        return;
    session_touch(s);
    g_string_append_len(s->in, data, len);
    session_pump(s);
}

/* 【Wi-Fi：TCP】 */

static void tcp_write_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Session *s = data;
    GError *e = NULL;

    s->writing = FALSE;
    if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(src), res, NULL, &e)) {
        DBG("[%s] 写失败：%s", s->peer, e->message);
        g_error_free(e);
        session_close(s);
    } else {
        tcp_write_next(s);
    }
    session_unref(s);
}

static void tcp_write_next(Session *s)
{
    GBytes *b;
    gsize n;
    const void *d;

    if (s->closed || s->writing)
        return;
    b = g_queue_pop_head(&s->outq);
    if (!b) {
        if (s->closing)
            session_close(s);
        return;
    }
    d = g_bytes_get_data(b, &n);
    s->writing = TRUE;
    /* GBytes 挂在 qdata 上随写完释放 */
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(s->conn)), d, n, G_PRIORITY_DEFAULT,
                                    s->cancel, tcp_write_done, session_ref(s));
    g_object_set_data_full(G_OBJECT(s->conn), "hisi-out", b, (GDestroyNotify)g_bytes_unref);
}

static void tcp_read(Session *s);

static void tcp_read_done(GObject *src, GAsyncResult *res, gpointer data)
{
    Session *s = data;
    GError *e = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &e);

    if (n <= 0) {
        if (e && !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            DBG("[%s] 读失败：%s", s->peer, e->message);
        s->eof = n == 0;
        g_clear_error(&e);
        if (!s->closed && !(s->eof && s->busy))
            session_end(s);
    } else {
        session_feed(s, s->rbuf, n);
        tcp_read(s);
    }
    session_unref(s);
}

static void tcp_read(Session *s)
{
    if (s->closed || s->closing)
        return;
    g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(s->conn)), s->rbuf, sizeof s->rbuf,
                              G_PRIORITY_DEFAULT, s->cancel, tcp_read_done, session_ref(s));
}

static gboolean tcp_incoming(GSocketService *svc, GSocketConnection *conn, GObject *src, gpointer data)
{
    GSocketAddress *addr = g_socket_connection_get_remote_address(conn, NULL);
    char *peer = NULL;
    Session *s;

    if (G_IS_INET_SOCKET_ADDRESS(addr)) {
        char *ip = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(addr)));
        peer = g_strdup_printf("%s:%u", ip, g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(addr)));
        g_free(ip);
    }
    g_clear_object(&addr);
    if (n_tcp >= MAX_TCP) {
        WARN("[%s] 同时连接太多，拒绝", peer ? peer : "?");
        g_free(peer);
        return FALSE;   /* 不留引用，连接随即关闭 */
    }
    s = session_new(KIND_TCP, peer ? peer : "tcp");
    g_free(peer);
    s->conn = g_object_ref(conn);
    s->cancel = g_cancellable_new();
    n_tcp++;
    DBG("[%s] 连上（Wi-Fi）", s->peer);
    tcp_read(s);
    return TRUE;
}

static void tcp_start(void)
{
    GSocketService *svc = g_socket_service_new();
    GError *e = NULL;

    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(svc), LINK_PORT, NULL, &e)) {
        WARN("监听 TCP %d 失败：%s（Wi-Fi 连接不可用，蓝牙照常）", LINK_PORT, e->message);
        g_error_free(e);
        g_object_unref(svc);
        return;
    }
    g_signal_connect(svc, "incoming", G_CALLBACK(tcp_incoming), NULL);
    g_socket_service_start(svc);
}

/* 【Wi-Fi：UDP 发现】手机广播 "HISI-PHONE? <pc_id>"，ID 对得上就单播回 "HISI-PHONE! <pc_id> <端口>" */

static gboolean udp_in(GSocket *sock, GIOCondition cond, gpointer data)
{
    static gint64 window;
    static int count;
    char buf[128];
    GSocketAddress *src = NULL;
    gssize n = g_socket_receive_from(sock, &src, buf, sizeof buf - 1, NULL, NULL);
    gint64 now = g_get_monotonic_time();

    if (n > 0 && src) {
        buf[n] = 0;
        g_strchomp(buf);
        if (now - window > G_USEC_PER_SEC) {
            window = now;
            count = 0;
        }
        if (pc_id[0] && g_str_has_prefix(buf, "HISI-PHONE? ") && !strcmp(buf + 12, pc_id) && ++count <= 20) {
            char *r = g_strdup_printf("HISI-PHONE! %s %d\n", pc_id, LINK_PORT);
            g_socket_send_to(sock, src, r, strlen(r), NULL, NULL);
            g_free(r);
            DBG("回应发现请求");
        }
    }
    g_clear_object(&src);
    return G_SOURCE_CONTINUE;
}

static void udp_start(void)
{
    GError *e = NULL;
    GSocket *sock = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &e);
    GInetAddress *any;
    GSocketAddress *addr;
    GSource *src;

    if (!sock) {
        WARN("建 UDP socket 失败：%s", e->message);
        g_error_free(e);
        return;
    }
    any = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
    addr = g_inet_socket_address_new(any, LINK_PORT);
    if (!g_socket_bind(sock, addr, TRUE, &e)) {
        WARN("绑定 UDP %d 失败：%s（手机自动发现不可用，二维码里的 IP 仍可直连）", LINK_PORT, e->message);
        g_error_free(e);
        g_object_unref(sock);
    } else {
        src = g_socket_create_source(sock, G_IO_IN, NULL);
        g_source_set_callback(src, (GSourceFunc)(GCallback)udp_in, NULL, NULL);
        g_source_attach(src, NULL);
        g_source_unref(src);
    }
    g_object_unref(addr);
    g_object_unref(any);
}

/* 【蓝牙 BLE：GATT 外设】 */

enum { OBJ_APP, OBJ_SVC, OBJ_RX, OBJ_TX, OBJ_ADV };

static const char ble_xml[] =
    "<node>"
    " <interface name='org.freedesktop.DBus.ObjectManager'>"
    "  <method name='GetManagedObjects'>"
    "   <arg type='a{oa{sa{sv}}}' name='objects' direction='out'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.bluez.GattService1'>"
    "  <property name='UUID' type='s' access='read'/>"
    "  <property name='Primary' type='b' access='read'/>"
    " </interface>"
    " <interface name='" CHRC_IFACE "'>"
    "  <method name='ReadValue'>"
    "   <arg type='a{sv}' name='options' direction='in'/>"
    "   <arg type='ay' name='value' direction='out'/>"
    "  </method>"
    "  <method name='WriteValue'>"
    "   <arg type='ay' name='value' direction='in'/>"
    "   <arg type='a{sv}' name='options' direction='in'/>"
    "  </method>"
    "  <method name='StartNotify'/>"
    "  <method name='StopNotify'/>"
    "  <property name='UUID' type='s' access='read'/>"
    "  <property name='Service' type='o' access='read'/>"
    "  <property name='Flags' type='as' access='read'/>"
    "  <property name='Notifying' type='b' access='read'/>"
    " </interface>"
    " <interface name='org.bluez.LEAdvertisement1'>"
    "  <method name='Release'/>"
    "  <property name='Type' type='s' access='read'/>"
    "  <property name='ServiceUUIDs' type='as' access='read'/>"
    "  <property name='Discoverable' type='b' access='read'/>"
    "  <property name='Duration' type='q' access='read'/>"
    " </interface>"
    "</node>";

static struct {
    char *adapter;            /* 已注册到的适配器 */
    gboolean app_ok, adv_ok;
    gboolean notifying;
} ble;

static GVariant *obj_prop(int obj, const char *prop)
{
    static const char *const rx_flags[] = { "write", "write-without-response", NULL };
    static const char *const tx_flags[] = { "notify", NULL };
    static const char *const uuids[] = { UUID_SVC, NULL };

    switch (obj) {
    case OBJ_SVC:
        if (!strcmp(prop, "UUID"))
            return g_variant_new_string(UUID_SVC);
        if (!strcmp(prop, "Primary"))
            return g_variant_new_boolean(TRUE);
        break;
    case OBJ_RX:
    case OBJ_TX:
        if (!strcmp(prop, "UUID"))
            return g_variant_new_string(obj == OBJ_RX ? UUID_RX : UUID_TX);
        if (!strcmp(prop, "Service"))
            return g_variant_new_object_path(BLE_SVC);
        if (!strcmp(prop, "Flags"))
            return g_variant_new_strv(obj == OBJ_RX ? rx_flags : tx_flags, -1);
        if (!strcmp(prop, "Notifying"))
            return g_variant_new_boolean(obj == OBJ_TX && ble.notifying);
        break;
    case OBJ_ADV:
        if (!strcmp(prop, "Type"))
            return g_variant_new_string("peripheral");
        if (!strcmp(prop, "ServiceUUIDs"))
            return g_variant_new_strv(uuids, -1);
        if (!strcmp(prop, "Discoverable"))
            return g_variant_new_boolean(TRUE);
        if (!strcmp(prop, "Duration"))
            return g_variant_new_uint16(ADV_DURATION_S);
        break;
    }
    return NULL;
}

static GVariant *obj_props(int obj, const char *const *names)
{
    GVariantBuilder b;

    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    for (; *names; names++)
        g_variant_builder_add(&b, "{sv}", *names, obj_prop(obj, *names));
    return g_variant_builder_end(&b);
}

static GVariant *managed_objects(void)
{
    static const char *const svc_props[] = { "UUID", "Primary", NULL };
    static const char *const chrc_props[] = { "UUID", "Service", "Flags", "Notifying", NULL };
    static const struct { const char *path, *iface; int obj; const char *const *props; } objs[] = {
        { BLE_SVC, "org.bluez.GattService1", OBJ_SVC, svc_props },
        { BLE_RX, CHRC_IFACE, OBJ_RX, chrc_props },
        { BLE_TX, CHRC_IFACE, OBJ_TX, chrc_props },
    };
    GVariantBuilder b;

    g_variant_builder_init(&b, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    for (size_t i = 0; i < G_N_ELEMENTS(objs); i++) {
        GVariantBuilder ifs;

        g_variant_builder_init(&ifs, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&ifs, "{s@a{sv}}", objs[i].iface, obj_props(objs[i].obj, objs[i].props));
        g_variant_builder_add(&b, "{o@a{sa{sv}}}", objs[i].path, g_variant_builder_end(&ifs));
    }
    return g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&b));
}

static void ble_send(Session *s, const char *data, gsize len)
{
    gsize chunk = CLAMP((gsize)(s->mtu > 3 ? s->mtu : 23) - 3, 20, 512);

    if (!ble.notifying)
        DBG("[%s] 手机还没打开通知，回复可能收不到", s->peer);
    for (gsize off = 0; off < len; off += chunk) {
        gsize n = MIN(chunk, len - off);
        GVariantBuilder ch;

        g_variant_builder_init(&ch, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&ch, "{sv}", "Value",
                              g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data + off, n, 1));
        g_dbus_connection_emit_signal(bus, NULL, BLE_TX, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                      g_variant_new("(sa{sv}as)", CHRC_IFACE, &ch, NULL), NULL);
    }
}

static void ble_write(GVariant *params, GDBusMethodInvocation *inv)
{
    GVariant *val, *opts;
    const char *dev = NULL;
    guint16 mtu = 0;
    const guint8 *d;
    gsize n;

    g_variant_get(params, "(@ay@a{sv})", &val, &opts);
    g_variant_lookup(opts, "device", "&o", &dev);
    g_variant_lookup(opts, "mtu", "q", &mtu);
    if (!dev)
        dev = "?";
    if (ble_sess && strcmp(ble_sess->dev, dev)) {
        if (g_get_monotonic_time() - ble_sess->last > BLE_BUSY_S * G_USEC_PER_SEC) {
            session_close(ble_sess);
        } else {
            DBG("另一台手机（%s）正在用蓝牙，拒绝 %s", ble_sess->dev, dev);
            g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.NotPermitted", "busy");
            goto out;
        }
    }
    if (!ble_sess) {
        const char *mac = strrchr(dev, '/');
        ble_sess = session_new(KIND_BLE, mac && g_str_has_prefix(mac, "/dev_") ? mac + 5 : dev);
        ble_sess->dev = g_strdup(dev);
        DBG("[%s] 连上（蓝牙）", ble_sess->peer);
    }
    if (mtu)
        ble_sess->mtu = mtu;
    g_dbus_method_invocation_return_value(inv, NULL);
    d = g_variant_get_fixed_array(val, &n, 1);
    session_feed(ble_sess, d, n);
out:
    g_variant_unref(val);
    g_variant_unref(opts);
}

static void ble_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                       const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    int obj = GPOINTER_TO_INT(data);

    if (obj == OBJ_APP && !strcmp(method, "GetManagedObjects")) {
        g_dbus_method_invocation_return_value(inv, managed_objects());
    } else if (obj == OBJ_RX && !strcmp(method, "WriteValue")) {
        ble_write(params, inv);
    } else if (obj == OBJ_TX && !strcmp(method, "StartNotify")) {
        ble.notifying = TRUE;
        DBG("手机打开了通知");
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (obj == OBJ_TX && !strcmp(method, "StopNotify")) {
        ble.notifying = FALSE;
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (obj == OBJ_ADV && !strcmp(method, "Release")) {
        INFO("BlueZ 收回了蓝牙广播");
        ble.adv_ok = FALSE;
        adv_lost();
        g_dbus_method_invocation_return_value(inv, NULL);
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.NotSupported", method);
    }
}

static GVariant *ble_get(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *prop, GError **err, gpointer data)
{
    GVariant *v = obj_prop(GPOINTER_TO_INT(data), prop);

    if (!v)
        g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "没有属性 %s", prop);
    return v;
}

static void ble_export(void)
{
    static const GDBusInterfaceVTable vt = { ble_method, ble_get, NULL };
    static const struct { const char *path; int iface, obj; } objs[] = {
        { BLE_APP, 0, OBJ_APP }, { BLE_SVC, 1, OBJ_SVC }, { BLE_RX, 2, OBJ_RX }, { BLE_TX, 2, OBJ_TX },
        { BLE_ADV, 3, OBJ_ADV },
    };
    GError *e = NULL;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(ble_xml, &e);

    for (size_t i = 0; node && i < G_N_ELEMENTS(objs); i++) {
        if (!g_dbus_connection_register_object(bus, objs[i].path, node->interfaces[objs[i].iface], &vt,
                                               GINT_TO_POINTER(objs[i].obj), NULL, &e)) {
            WARN("导出 %s 失败：%s", objs[i].path, e->message);
            g_clear_error(&e);
        }
    }
    if (node)
        g_dbus_node_info_unref(node);
    else
        WARN("解析 GATT 接口描述失败：%s", e->message);
}

static void reg_app_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(bus, res, &e);

    if (r) {
        ble.app_ok = TRUE;
        INFO("蓝牙 GATT 服务已注册（%s）", ble.adapter);
        g_variant_unref(r);
    } else {
        ble.app_ok = g_str_has_suffix(e->message, "Already Exists") || strstr(e->message, "AlreadyExists");
        if (!ble.app_ok)
            WARN("注册蓝牙 GATT 服务失败：%s", e->message);
        g_error_free(e);
    }
}

/*
 * 【蓝牙广播续期】
 * L540 的 5.4 内核走扩展广播时，给控制器的每个广播实例都带“时长”（不给就默认 2 秒，按毫秒存进 u16，
 * 最多 65 秒），控制器到时报 Advertising Timeout，内核不理这个事件也不重开——BlueZ 里还显示在广播，
 * 空中早没了，手机扫不到也直连不上。所以时长设 60 秒，到期后注销再注册续上。
 * 坑：内核注销时发“停止全部广播”，这块控制器回成功却不停，旧实例照播到时长用完，期间重新注册
 * 设参数被拒（Command Disallowed，BlueZ 只报 Failed）。所以续期要等旧实例到期：
 *   - 自己注册成功的，知道到期时刻，到点（再等 ADV_SLACK_MS）续；
 *   - 手机断开时内核会自己重开一个整时长的广播：先试一次，不行就等到那个到期时刻；
 *   - 别的情况（刚启动时上一个进程的广播还在播、BlueZ 说早注册着等）不知道到期时刻，每秒试一次，
 *     最多一整个时长，旧实例那期间一直在播，不会断档。
 * 只有一个定时器 adv_timer、同一时刻只有一个注册/注销请求在途（adv_busy）。手机连着时不动它。
 */
static void ble_refresh(void);
static guint adv_timer;
static gboolean adv_busy;
static gint64 adv_expire;         /* 控制器上当前广播预计到期的单调时间（微秒），0 = 不知道 */
static int adv_fails;             /* 不知道到期时刻时连续注册失败的次数 */
static gboolean adv_announced, adv_warned;

static gboolean adv_tick(gpointer data);

static void adv_stop(void)
{
    if (adv_timer) {
        g_source_remove(adv_timer);
        adv_timer = 0;
    }
}

static void adv_at(gint64 when)
{
    gint64 now = g_get_monotonic_time();

    adv_stop();
    adv_timer = g_timeout_add(when > now ? (guint)((when - now + 999) / 1000) : 0, adv_tick, NULL);
}

static void adv_in(guint ms)
{
    adv_at(g_get_monotonic_time() + (gint64)ms * 1000);
}

static void reg_adv_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(bus, res, &e);
    gint64 now = g_get_monotonic_time();

    adv_busy = FALSE;
    if (r) {
        g_variant_unref(r);
        ble.adv_ok = TRUE;
        adv_expire = now + ADV_DURATION_S * G_USEC_PER_SEC;
        adv_at(adv_expire + ADV_SLACK_MS * 1000);
        if (!adv_announced || adv_warned)
            INFO("蓝牙广播已开始（控制器每次播 %d 秒，到期自动续）", ADV_DURATION_S);
        else
            DBG("蓝牙广播已续上%s", adv_fails ? "（等到旧广播到期）" : "");
        adv_announced = TRUE;
        adv_warned = FALSE;
        adv_fails = 0;
    } else if (strstr(e->message, "AlreadyExists") || strstr(e->message, "Already Exists")) {
        /* BlueZ 那边早注册着：不知道它哪会儿开播的，注销重来一次对上节拍 */
        DBG("蓝牙广播早已注册，重新注册一次");
        ble.adv_ok = TRUE;
        adv_expire = 0;
        adv_in(ADV_RETRY_MS);
        g_error_free(e);
    } else {
        if (!ble.adapter) {
            DBG("开始蓝牙广播失败（适配器已不在）：%s", e->message);
        } else if (adv_expire > now) {
            DBG("开始蓝牙广播失败，旧广播还有 %lld 毫秒到期，到时再试：%s",
                (long long)((adv_expire - now) / 1000), e->message);
            adv_at(adv_expire + ADV_SLACK_MS * 1000);
            adv_expire = 0;                       /* 到点还不行就当不知道，改每秒试 */
        } else if (++adv_fails < ADV_RETRY_MAX) {
            DBG("开始蓝牙广播失败（第 %d 次），%d 毫秒后再试：%s", adv_fails, ADV_RETRY_MS, e->message);
            adv_in(ADV_RETRY_MS);
        } else {
            if (!adv_warned)
                WARN("开始蓝牙广播失败：%s（之后每 %d 秒再试）", e->message, ADV_SLOW_S);
            adv_warned = TRUE;
            adv_in(ADV_SLOW_S * 1000);
        }
        g_error_free(e);
    }
}

static void adv_register(void)
{
    if (adv_busy || !ble.adapter || ble.adv_ok)
        return;
    adv_busy = TRUE;
    g_dbus_connection_call(bus, "org.bluez", ble.adapter, "org.bluez.LEAdvertisingManager1",
                           "RegisterAdvertisement", g_variant_new("(oa{sv})", BLE_ADV, NULL), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 10000, NULL, reg_adv_done, NULL);
}

static void unreg_adv_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GVariant *r = g_dbus_connection_call_finish(bus, res, NULL);

    if (r)
        g_variant_unref(r);
    adv_busy = FALSE;
    ble.adv_ok = FALSE;
    ble_refresh();                                /* 顺带确认适配器还在、还上着电，再注册 */
}

/* 续一次：注册着就先注销再注册，没注册就直接注册 */
static void adv_renew(void)
{
    if (adv_busy)
        return;                                   /* 在途的请求回来时会自己排下一步 */
    if (!ble.adv_ok || !ble.adapter) {
        ble_refresh();
        return;
    }
    adv_busy = TRUE;
    g_dbus_connection_call(bus, "org.bluez", ble.adapter, "org.bluez.LEAdvertisingManager1",
                           "UnregisterAdvertisement", g_variant_new("(o)", BLE_ADV), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 10000, NULL, unreg_adv_done, NULL);
}

static gboolean adv_tick(gpointer data)
{
    adv_timer = 0;
    if (!ble_sess)                                /* 手机连着时不动；断开时 adv_session_gone 接着续 */
        adv_renew();
    return G_SOURCE_REMOVE;
}

/* 有蓝牙设备断开：内核会按“连接打断了广播”重开当前实例，又是一整个时长 */
static void adv_disconnected(void)
{
    gint64 t = g_get_monotonic_time() + ADV_DURATION_S * G_USEC_PER_SEC;

    if (t > adv_expire)
        adv_expire = t;
    adv_fails = 0;
}

/* 手机会话结束（多半是断开了）：马上试一次——内核没重开（比如连上时我们正好没注册着）就直接续上；
 * 重开了的话注册会被拒，按 adv_disconnected 记的到期时刻再续 */
static void adv_session_gone(void)
{
    if (!ble.adapter)
        return;
    adv_disconnected();
    adv_stop();
    adv_renew();
}

/* BlueZ 收回了广播（比如适配器断电）：过一会儿重新找适配器注册 */
static void adv_lost(void)
{
    adv_expire = 0;
    adv_fails = 0;
    if (!adv_busy)
        adv_in(ADV_RETRY_MS);
}

static gboolean has_iface(GVariant *ifaces, const char *name)
{
    GVariant *v = g_variant_lookup_value(ifaces, name, NULL);

    if (v)
        g_variant_unref(v);
    return v != NULL;
}

/* 找第一个已上电、能做 GATT 外设和广播的适配器，注册上去（已注册的不重复） */
static void ble_refresh(void)
{
    GVariant *r, *ifaces;
    GVariantIter *it;
    const char *path;
    char *found = NULL;

    r = g_dbus_connection_call_sync(bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                                    NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    if (!r)
        return;
    g_variant_get(r, "(a{oa{sa{sv}}})", &it);
    while (!found && g_variant_iter_next(it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *ad = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
        gboolean powered = FALSE;

        if (ad) {
            g_variant_lookup(ad, "Powered", "b", &powered);
            if (powered && has_iface(ifaces, "org.bluez.GattManager1")
                && has_iface(ifaces, "org.bluez.LEAdvertisingManager1"))
                found = g_strdup(path);
            g_variant_unref(ad);
        }
        g_variant_unref(ifaces);
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    if (!found) {
        DBG("没有已上电的蓝牙适配器");
        return;
    }
    if (g_strcmp0(found, ble.adapter)) {
        g_free(ble.adapter);
        ble.adapter = found;
        ble.app_ok = ble.adv_ok = FALSE;
    } else {
        g_free(found);
    }
    if (!ble.app_ok)
        g_dbus_connection_call(bus, "org.bluez", ble.adapter, "org.bluez.GattManager1", "RegisterApplication",
                               g_variant_new("(oa{sv})", BLE_APP, NULL), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
                               reg_app_done, NULL);
    adv_register();
}

static void bluez_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer data)
{
    DBG("BlueZ 在线");
    ble.app_ok = ble.adv_ok = FALSE;
    g_clear_pointer(&ble.adapter, g_free);
    adv_expire = 0;
    adv_fails = 0;
    ble_refresh();
}

static void bluez_vanished(GDBusConnection *c, const char *name, gpointer data)
{
    if (ble.adapter)
        INFO("BlueZ 不在了，蓝牙连接暂不可用");
    ble.app_ok = ble.adv_ok = ble.notifying = FALSE;
    g_clear_pointer(&ble.adapter, g_free);
    adv_stop();
    away_clear("BlueZ 不在了");
    if (ble_sess)
        session_close(ble_sess);
}

/* 适配器上电 / 新出现：重新注册；手机断开：结束它的会话 */
static void bluez_props(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *member, GVariant *params, gpointer data)
{
    const char *which;
    GVariant *changed;
    gboolean v;

    if (!strcmp(member, "InterfacesAdded")) {
        GVariant *ifaces = g_variant_get_child_value(params, 1);

        if (has_iface(ifaces, "org.bluez.Adapter1") || has_iface(ifaces, "org.bluez.LEAdvertisingManager1"))
            ble_refresh();
        g_variant_unref(ifaces);
        return;
    }
    g_variant_get(params, "(&s@a{sv}@as)", &which, &changed, NULL);
    if (!strcmp(which, "org.bluez.Adapter1") && g_variant_lookup(changed, "Powered", "b", &v)) {
        if (v) {
            DBG("蓝牙适配器上电");
            ble.adv_ok = FALSE;                   /* BlueZ 可能还留着，注册会回 AlreadyExists，再重来一次 */
            adv_expire = 0;
            adv_fails = 0;
            ble_refresh();
        } else {
            ble.notifying = FALSE;
            away_clear("蓝牙适配器关了");
            if (ble_sess)
                session_close(ble_sess);
            adv_stop();
        }
    } else if (!strcmp(which, "org.bluez.Device1") && g_variant_lookup(changed, "Connected", "b", &v) && !v) {
        /* 不管是谁断开，内核都可能刚重开了一个整时长的广播：下次注册被拒就等到它到期，不用每秒试 */
        adv_disconnected();
        away_disconnected(path);
        if (ble_sess && !g_strcmp0(ble_sess->dev, path)) {
            session_close(ble_sess);
        } else if (!ble.adv_ok && ble.adapter && !ble_sess) {
            /* 正等旧广播到期（已注销）时有人连上又断开：内核没实例可重开，空中这会儿多半没广播，马上试 */
            DBG("等广播到期期间有设备断开，马上重新注册");
            adv_stop();
            adv_renew();
        }
    }
    g_variant_unref(changed);
}

/* 【hisi-bio-unlock 上下线】 */

static void link_info_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(bus, res, &e);
    const char *id, *host;

    if (!r) {
        WARN("取电脑 ID 失败：%s（D-Bus 策略没装或 hisi-bio-unlock 太旧？）", e->message);
        g_error_free(e);
        return;
    }
    g_variant_get(r, "(&s&s)", &id, &host);
    if (valid_id(id)) {
        g_strlcpy(pc_id, id, sizeof pc_id);
        INFO("电脑 %s（ID %.8s…）：Wi-Fi 端口 %d，蓝牙%s", host, pc_id, LINK_PORT, ble.app_ok ? "已就绪" : "等待中");
    }
    g_variant_unref(r);
}

static void bio_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer data)
{
    bio_up = TRUE;
    g_dbus_connection_call(bus, UI_SVC, UI_PATH, PHONE_IFACE, "GetLinkInfo", NULL, G_VARIANT_TYPE("(ss)"),
                           G_DBUS_CALL_FLAGS_NONE, 10000, NULL, link_info_done, NULL);
}

static void bio_vanished(GDBusConnection *c, const char *name, gpointer data)
{
    if (bio_up)
        WARN("hisi-bio-unlock 不在了，暂时不能解锁");
    bio_up = FALSE;
}

static gboolean on_term(gpointer data)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    GError *e = NULL;

    verbose = argc > 1 && !strcmp(argv[1], "-v");
    signal(SIGPIPE, SIG_IGN);
    aways = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, away_free);
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
    if (!bus) {
        LOG(3, "连不上 system bus：%s", e->message);
        return 1;
    }
    g_bus_watch_name_on_connection(bus, UI_SVC, G_BUS_NAME_WATCHER_FLAGS_NONE, bio_appeared, bio_vanished,
                                   NULL, NULL);
    tcp_start();
    udp_start();
    ble_export();
    g_dbus_connection_signal_subscribe(bus, "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                       NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, bluez_props, NULL, NULL);
    g_dbus_connection_signal_subscribe(bus, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                                       NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, bluez_props, NULL, NULL);
    g_bus_watch_name_on_connection(bus, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE, bluez_appeared, bluez_vanished,
                                   NULL, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_term, NULL);
    g_unix_signal_add(SIGINT, on_term, NULL);
    INFO("已启动");
    g_main_loop_run(loop);
    return 0;
}
