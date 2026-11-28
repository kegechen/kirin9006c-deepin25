/*
 * 手机指纹解锁的 root 侧（编进 hisi-bio-unlock）：配对库、一次性挑战、验签、加密电脑信息、截锁屏图，
 * 以及手机发起的锁屏（LOCK）和离开自动锁屏（AWAY 开关 + hisi-phone-link 报蓝牙断开后计时锁屏）。
 *
 * hisi-phone-link（用户 hisi-phone）只做 Wi-Fi / 蓝牙收发，经 D-Bus 调这里的 org.hisi.BioUnlock1.Phone；
 * 配对码、共享密钥、验签都只在本进程里。配对命令（root）走 org.hisi.BioUnlock1.Admin。
 * 协议、字节拼接方式见 docs/phone-unlock.md。
 *
 * 配对库 /var/lib/hisi-bio/phones.ini（root 0600）：
 *   [host] id=<pc_id>
 *   [phone <phone_id>] user= name= pub=<SPKI DER base64> secret=<S base64> paired=<unix 时间>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/x509.h>
#include "hisi-bio.h"

#define LINK_USER      "hisi-phone"
#define CHAL_TTL_US    (120 * G_USEC_PER_SEC)
#define MAX_CHALS      8
#define CHALS_PER_PHONE 2                        /* App 的后台蓝牙连接和解锁界面可能各要一个 */
#define AWAY_MIN_S     5                         /* 离开自动锁屏：蓝牙断开后等待秒数的范围 */
#define AWAY_MAX_S     600
#define FAIL_MAX       5                         /* 签名连错这么多次 */
#define FAIL_HOLD_US   (300 * G_USEC_PER_SEC)    /* 就暂停这么久 */
#define HELLO_PER_MIN  30                        /* 所有手机合计每分钟最多这么多次 HELLO */
#define PAIR_MIN_S     30
#define PAIR_MAX_S     600
#define PAIR_BAD_MAX   5
#define NAME_MAX_LEN   64
#define SHOT_TIMEOUT_S 8
#define SHOT_MAX       (512 * 1024)
#define SHOT_BASE_W    1440.0                    /* grim -s 1 时的宽度（逻辑像素，L540 是 1440x960） */
#define SHOT_MIN_W     288
#define SHOT_MAX_W     960

typedef struct {
    char *id, *user, *name;
    char *alias;           /* 用户起的别名，只管显示；NULL = 显示 name（手机配对时上报名） */
    GBytes *pub;           /* EC P-256 公钥，X.509 SubjectPublicKeyInfo DER */
    guint8 secret[32];     /* 配对时由配对码导出的 S */
    gint64 paired;
    int fails;
    gint64 hold_until;
    guint away;            /* 离开自动锁屏：0 = 没开，否则是蓝牙断开后等待的秒数。只在内存里 */
    guint away_timer;      /* 蓝牙断开后的锁屏倒计时 */
} Phone;

/* 给用户看的名：有别名用别名，没有就用手机上报名 */
static const char *p_disp(const Phone *p)
{
    return p->alias ? p->alias : p->name;
}

typedef struct {
    char phone_id[33];
    guint8 cn[32], pn[16], ks[32];
    gint64 expires;        /* 0 = 空槽 */
} Chal;

static GHashTable *phones;   /* phone_id → Phone* */
static GHashTable *online;   /* 现在连着电脑的 phone_id → gint64*（连上的单调时间，微秒） */
static char pc_id[33];
static uid_t link_uid = (uid_t)-1;
static Chal chals[MAX_CHALS];
static struct {
    gboolean open;
    char *user;
    guint8 code[16];
    guint timer;
    int bad;
    char *owner;           /* 经 User 接口（控制中心）开始配对的客户端的总线名；sudo hisi-phone pair 开的是 NULL */
    guint owner_watch;     /* 那个客户端退出就取消配对 */
} pairing;

static void pair_close(const char *result, const char *name);
static void phones_changed(void);
static struct {
    gint64 window;
    int count;
} hello_rate;
static gboolean shot_busy;

/* 【小工具】 */

static void rand_bytes(void *buf, size_t n)
{
    guint8 *p = buf;

    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            g_error("getrandom 失败：%s", g_strerror(errno));
        }
        p += r;
        n -= r;
    }
}

static void to_hex(const guint8 *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
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

static void put(GByteArray *a, const void *p, size_t n)
{
    g_byte_array_append(a, p, n);
}

/* 字符串 + 一个 0x00 */
static void put0(GByteArray *a, const char *s)
{
    put(a, s, strlen(s) + 1);
}

static void hmac(const void *key, size_t klen, const GByteArray *msg, guint8 out[32])
{
    unsigned int len = 32;

    HMAC(EVP_sha256(), key, (int)klen, msg->data, msg->len, out, &len);
}

/* box(k, aad, p) = iv(12) ‖ AES-256-GCM 密文 ‖ tag(16) */
static GVariant *seal(const guint8 key[32], const char *aad, const void *data, gsize len)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    guint8 *out = g_malloc(12 + len + 16);
    int n = 0, f = 0;
    gboolean ok;

    rand_bytes(out, 12);
    ok = c && EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1
        && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1
        && EVP_EncryptInit_ex(c, NULL, NULL, key, out) == 1
        && EVP_EncryptUpdate(c, NULL, &n, (const guint8 *)aad, (int)strlen(aad)) == 1
        && EVP_EncryptUpdate(c, out + 12, &n, data, (int)len) == 1
        && EVP_EncryptFinal_ex(c, out + 12 + n, &f) == 1
        && (gsize)(n + f) == len
        && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out + 12 + len) == 1;
    EVP_CIPHER_CTX_free(c);
    if (!ok) {
        WARN("AES-GCM 加密失败");
        g_free(out);
        return NULL;
    }
    return g_variant_new_from_data(G_VARIANT_TYPE_BYTESTRING, out, 12 + len + 16, TRUE, g_free, out);
}

static GVariant *empty_ay(void)
{
    return g_variant_new_from_data(G_VARIANT_TYPE_BYTESTRING, "", 0, TRUE, NULL, NULL);
}

/* 只收 EC P-256 公钥 */
static EVP_PKEY *parse_pub(const guint8 *der, gsize len)
{
    const unsigned char *p = der;
    EVP_PKEY *k = d2i_PUBKEY(NULL, &p, (long)len);
    char g[32] = "";
    size_t gl = 0;

    if (!k)
        return NULL;
    if (p != der + len || !EVP_PKEY_is_a(k, "EC") || !EVP_PKEY_get_group_name(k, g, sizeof g, &gl)
        || strcmp(g, "prime256v1")) {
        EVP_PKEY_free(k);
        return NULL;
    }
    return k;
}

static gboolean verify(GBytes *pub, const GByteArray *msg, const guint8 *sig, gsize siglen)
{
    gsize n;
    const guint8 *d = g_bytes_get_data(pub, &n);
    EVP_PKEY *k = parse_pub(d, n);
    EVP_MD_CTX *c;
    gboolean ok;

    if (!k)
        return FALSE;
    c = EVP_MD_CTX_new();
    ok = c && EVP_DigestVerifyInit(c, NULL, EVP_sha256(), NULL, k) == 1
        && EVP_DigestVerify(c, sig, siglen, msg->data, msg->len) == 1;
    EVP_MD_CTX_free(c);
    EVP_PKEY_free(k);
    return ok;
}

static const guint8 *ay(GVariant *v, gsize *len)
{
    return g_variant_get_fixed_array(v, len, 1);
}

/* 【配对库】 */

static void phone_free(gpointer data)
{
    Phone *p = data;

    if (p->away_timer)
        g_source_remove(p->away_timer);
    g_free(p->id);
    g_free(p->user);
    g_free(p->name);
    g_free(p->alias);
    if (p->pub)
        g_bytes_unref(p->pub);
    OPENSSL_cleanse(p->secret, sizeof p->secret);
    g_free(p);
}

static void store_save(void)
{
    GKeyFile *kf = g_key_file_new();
    GHashTableIter it;
    Phone *p;
    GError *e = NULL;
    char *data;
    gsize len;

    g_key_file_set_string(kf, "host", "id", pc_id);
    g_hash_table_iter_init(&it, phones);
    while (g_hash_table_iter_next(&it, NULL, (gpointer *)&p)) {
        char *grp = g_strconcat("phone ", p->id, NULL);
        char *b64;

        g_key_file_set_string(kf, grp, "user", p->user);
        g_key_file_set_string(kf, grp, "name", p->name);
        if (p->alias)
            g_key_file_set_string(kf, grp, "alias", p->alias);
        b64 = g_base64_encode(g_bytes_get_data(p->pub, NULL), g_bytes_get_size(p->pub));
        g_key_file_set_string(kf, grp, "pub", b64);
        g_free(b64);
        b64 = g_base64_encode(p->secret, sizeof p->secret);
        g_key_file_set_string(kf, grp, "secret", b64);
        OPENSSL_cleanse(b64, strlen(b64));
        g_free(b64);
        g_key_file_set_int64(kf, grp, "paired", p->paired);
        g_free(grp);
    }
    data = g_key_file_to_data(kf, &len, NULL);
    if (!g_file_set_contents_full(PHONE_STORE, data, len, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &e)) {
        WARN("写 %s 失败：%s", PHONE_STORE, e->message);
        g_error_free(e);
    }
    OPENSSL_cleanse(data, len);
    g_free(data);
    g_key_file_free(kf);
}

static void store_load(void)
{
    GKeyFile *kf = g_key_file_new();
    GError *e = NULL;
    char **groups, *s;

    if (!g_key_file_load_from_file(kf, PHONE_STORE, G_KEY_FILE_NONE, &e)) {
        if (!g_error_matches(e, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            WARN("读 %s 失败：%s（当作没有配对过的手机）", PHONE_STORE, e->message);
        g_error_free(e);
        g_key_file_free(kf);
        return;
    }
    s = g_key_file_get_string(kf, "host", "id", NULL);
    if (s && valid_id(s))
        g_strlcpy(pc_id, s, sizeof pc_id);
    g_free(s);
    groups = g_key_file_get_groups(kf, NULL);
    for (char **g = groups; *g; g++) {
        Phone *p;
        char *pub, *sec;
        guchar *raw;
        gsize n = 0;

        if (!g_str_has_prefix(*g, "phone ") || !valid_id(*g + 6))
            continue;
        p = g_new0(Phone, 1);
        p->id = g_strdup(*g + 6);
        p->user = g_key_file_get_string(kf, *g, "user", NULL);
        p->name = g_key_file_get_string(kf, *g, "name", NULL);
        p->alias = g_key_file_get_string(kf, *g, "alias", NULL);
        if (p->alias && !*p->alias) {
            g_free(p->alias);
            p->alias = NULL;
        }
        p->paired = g_key_file_get_int64(kf, *g, "paired", NULL);
        pub = g_key_file_get_string(kf, *g, "pub", NULL);
        sec = g_key_file_get_string(kf, *g, "secret", NULL);
        if (pub) {
            raw = g_base64_decode(pub, &n);
            p->pub = g_bytes_new_take(raw, n);
        }
        if (sec) {
            raw = g_base64_decode(sec, &n);
            if (n == sizeof p->secret)
                memcpy(p->secret, raw, n);
            OPENSSL_cleanse(raw, n);
            g_free(raw);
        }
        if (!p->user || !p->name || !p->pub || !sec || n != sizeof p->secret) {
            WARN("%s 里 [%s] 不完整，忽略", PHONE_STORE, *g);
            phone_free(p);
        } else {
            g_hash_table_replace(phones, p->id, p);
        }
        g_free(pub);
        if (sec)
            OPENSSL_cleanse(sec, strlen(sec));
        g_free(sec);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
}

/* 【挑战】 */

static Chal *chal_find(const char *phone_id, GVariant *cn)
{
    gint64 now = g_get_monotonic_time();
    gsize n;
    const guint8 *c = ay(cn, &n);

    if (n != 32)
        return NULL;
    for (int i = 0; i < MAX_CHALS; i++)
        if (chals[i].expires > now && !strcmp(chals[i].phone_id, phone_id) && !CRYPTO_memcmp(chals[i].cn, c, 32))
            return &chals[i];
    return NULL;
}

/* 同一台手机最多留 CHALS_PER_PHONE 个挑战，再多就挤掉它自己最旧的；没有空槽就挤掉全部里最旧的 */
static Chal *chal_slot(const char *phone_id)
{
    gint64 now = g_get_monotonic_time();
    Chal *oldest = &chals[0], *mine = NULL, *empty = NULL;
    int n = 0;

    for (int i = 0; i < MAX_CHALS; i++) {
        Chal *c = &chals[i];

        if (c->expires <= now) {
            if (!empty)
                empty = c;
            continue;
        }
        if (!strcmp(c->phone_id, phone_id)) {
            n++;
            if (!mine || c->expires < mine->expires)
                mine = c;
        }
        if (c->expires < oldest->expires)
            oldest = c;
    }
    if (n >= CHALS_PER_PHONE)
        return mine;
    return empty ? empty : oldest;
}

static void chal_drop(Chal *c)
{
    OPENSSL_cleanse(c, sizeof *c);
}

/* 【截锁屏图】降权到该用户，grim 截 PPM，cjpeg 压成 JPEG */

typedef struct {
    GDBusMethodInvocation *inv;
    guint8 ks[32];
    GSubprocess *proc;
    GCancellable *cancel;
    guint timer;
} ShotJob;

typedef struct {
    uid_t uid;
    gid_t gid;
} Creds;

static void drop_privs(gpointer data)
{
    const Creds *c = data;

    if (setgroups(0, NULL) || setgid(c->gid) || setuid(c->uid))
        _exit(127);
}

/* 该用户的 wayland socket（treeland 给每个用户会话建的），没有返回 NULL */
static char *wayland_socket(uid_t uid)
{
    static const char *const names[] = { "treeland.socket", "wayland-0" };

    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        char *p = g_strdup_printf("/run/user/%u/%s", (unsigned)uid, names[i]);
        gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS);

        g_free(p);
        if (ok)
            return g_strdup(names[i]);
    }
    return NULL;
}

static gboolean shot_possible(const char *user)
{
    struct passwd *pw = getpwnam(user);
    char *sock;

    if (!pw || !g_file_test("/usr/bin/grim", G_FILE_TEST_IS_EXECUTABLE)
        || !g_file_test("/usr/bin/cjpeg", G_FILE_TEST_IS_EXECUTABLE))
        return FALSE;
    sock = wayland_socket(pw->pw_uid);
    g_free(sock);
    return sock != NULL;
}

static void shot_finish(ShotJob *j, int status, GVariant *box)
{
    if (j->timer)
        g_source_remove(j->timer);
    g_dbus_method_invocation_return_value(j->inv, g_variant_new("(i@ay)", status, box ? box : empty_ay()));
    g_clear_object(&j->proc);
    g_clear_object(&j->cancel);
    OPENSSL_cleanse(j->ks, sizeof j->ks);
    g_free(j);
    shot_busy = FALSE;
}

static gboolean shot_timeout(gpointer data)
{
    ShotJob *j = data;

    j->timer = 0;
    WARN("截图超时");
    /* 杀整个进程组（setsid 起的 sh + grim + cjpeg），只杀 sh 会留下卡住的 grim */
    pid_t pid = g_subprocess_get_identifier(j->proc) ? atoi(g_subprocess_get_identifier(j->proc)) : 0;

    if (pid > 0)
        kill(-pid, SIGKILL);   /* 杀整个进程组（setsid 起的 sh + grim + cjpeg） */
    g_subprocess_force_exit(j->proc);
    g_cancellable_cancel(j->cancel);
    return G_SOURCE_REMOVE;
}

static void shot_done(GObject *src, GAsyncResult *res, gpointer data)
{
    ShotJob *j = data;
    GBytes *out = NULL;
    GError *e = NULL;
    const guint8 *d;
    gsize n = 0;

    if (!g_subprocess_communicate_finish(j->proc, res, &out, NULL, &e)) {
        WARN("截图失败：%s", e->message);
        g_error_free(e);
        shot_finish(j, PS_ERROR, NULL);
        return;
    }
    d = out ? g_bytes_get_data(out, &n) : NULL;
    if (!g_subprocess_get_successful(j->proc) || n < 4 || n > SHOT_MAX || d[0] != 0xff || d[1] != 0xd8) {
        WARN("截图失败（退出码 %d，%zu 字节）", g_subprocess_get_if_exited(j->proc)
             ? g_subprocess_get_exit_status(j->proc) : -1, n);
        shot_finish(j, PS_ERROR, NULL);
    } else {
        GVariant *box = seal(j->ks, "hisi-shot-v1", d, n);
        DBG("截图 %zu 字节", n);
        shot_finish(j, box ? PS_OK : PS_ERROR, box);
    }
    if (out)
        g_bytes_unref(out);
}

/* 成功时接管 inv，返回 PS_OK；否则返回错误码，由调用方回复 */
static int shot_start(GDBusMethodInvocation *inv, const char *user, const guint8 ks[32], guint width)
{
    struct passwd *pw = getpwnam(user);
    GSubprocessLauncher *l;
    GError *e = NULL;
    ShotJob *j;
    Creds creds;
    char *sock, *env[6], scale[16];

    if (shot_busy)
        return PS_BUSY;
    if (!pw || !(sock = wayland_socket(pw->pw_uid)))
        return PS_ERROR;
    width = CLAMP(width, SHOT_MIN_W, SHOT_MAX_W);
    g_ascii_formatd(scale, sizeof scale, "%.3f", width / SHOT_BASE_W);
    env[0] = g_strdup("PATH=/usr/bin:/bin");
    env[1] = g_strdup("HOME=/");
    env[2] = g_strdup("LANG=C");
    env[3] = g_strdup_printf("XDG_RUNTIME_DIR=/run/user/%u", (unsigned)pw->pw_uid);
    env[4] = g_strconcat("WAYLAND_DISPLAY=", sock, NULL);
    env[5] = NULL;
    creds.uid = pw->pw_uid;
    creds.gid = pw->pw_gid;
    l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_environ(l, env);
    g_subprocess_launcher_set_cwd(l, "/");
    g_subprocess_launcher_set_child_setup(l, drop_privs, &creds, NULL);
    j = g_new0(ShotJob, 1);
    /* 缩放比例作为位置参数传进去，不拼进脚本 */
    /* setsid 让 grim / cjpeg 和 sh 同组，超时时一起杀掉（不然 grim 卡住会不断堆积） */
    j->proc = g_subprocess_launcher_spawn(l, &e, "/usr/bin/setsid", "sh", "-c", "grim -t ppm -s \"$1\" - | cjpeg -quality 60",
                                          "sh", scale, NULL);
    g_object_unref(l);
    for (int i = 0; env[i]; i++)
        g_free(env[i]);
    g_free(sock);
    if (!j->proc) {
        WARN("启动截图失败：%s", e->message);
        g_error_free(e);
        g_free(j);
        return PS_ERROR;
    }
    j->inv = inv;
    memcpy(j->ks, ks, 32);
    j->cancel = g_cancellable_new();
    j->timer = g_timeout_add_seconds(SHOT_TIMEOUT_S, shot_timeout, j);
    shot_busy = TRUE;
    g_subprocess_communicate_async(j->proc, NULL, j->cancel, shot_done, j);
    return PS_OK;
}

/* 【org.hisi.BioUnlock1.Phone：hisi-phone-link 调用】 */

static char *clean_line(const char *s)
{
    char *r = g_strdup(s ? s : "");

    for (char *p = r; *p; p++)
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    return r;
}

static char *info_text(const Phone *p)
{
    struct passwd *pw = getpwnam(p->user);
    GString *s = g_string_new(NULL);
    gboolean has, locked;
    gint64 secs;
    char host[256] = "", *v;

    gethostname(host, sizeof host - 1);
    bio_session_info(p->user, &has, &locked, &secs);
    g_string_append_printf(s, "user=%s\n", p->user);
    if (pw && pw->pw_gecos) {
        char *rn = g_strndup(pw->pw_gecos, strcspn(pw->pw_gecos, ","));
        v = clean_line(rn);
        g_string_append_printf(s, "realname=%s\n", v);
        g_free(v);
        g_free(rn);
    }
    v = clean_line(host);
    g_string_append_printf(s, "host=%s\nsession=%d\nlocked=%d\n", v, has, locked);
    g_free(v);
    if (locked)
        g_string_append_printf(s, "locked_for=%" G_GINT64_FORMAT "\n", secs);
    g_string_append_printf(s, "shot=%d\naway=%u\n", locked && shot_possible(p->user), p->away);
    return g_string_free(s, FALSE);
}

static guint user_phone_count(const char *user)
{
    GHashTableIter it;
    Phone *p;
    guint n = 0;

    g_hash_table_iter_init(&it, phones);
    while (g_hash_table_iter_next(&it, NULL, (gpointer *)&p))
        if (!g_strcmp0(p->user, user))
            n++;
    return n;
}

static gboolean held(Phone *p)
{
    return p->hold_until && g_get_monotonic_time() < p->hold_until;
}

static void m_pair(GVariant *params, GDBusMethodInvocation *inv)
{
    const char *id, *name;
    GVariant *vpub, *vmac;
    const guint8 *pub, *mac;
    gsize publen, maclen;
    GByteArray *t = NULL, *m;
    guint8 want[32], mac2[32];
    int status = PS_BAD_REQUEST;
    EVP_PKEY *k;
    Phone *p, *old;

    g_variant_get(params, "(&s&s@ay@ay)", &id, &name, &vpub, &vmac);
    pub = ay(vpub, &publen);
    mac = ay(vmac, &maclen);
    if (!pairing.open) {
        status = PS_PAIR_CLOSED;
        goto out;
    }
    if (!valid_id(id) || !*name || strlen(name) > NAME_MAX_LEN || maclen != 32 || !publen || publen > 512)
        goto out;
    for (const char *c = name; *c; c++)
        if ((guchar)*c < 0x20)
            goto out;
    if (!(k = parse_pub(pub, publen)))
        goto out;
    EVP_PKEY_free(k);
    t = g_byte_array_new();
    put0(t, "hisi-phone-pair-v1");
    put0(t, pc_id);
    put0(t, id);
    put0(t, name);
    put(t, pub, publen);
    hmac(pairing.code, sizeof pairing.code, t, want);
    if (CRYPTO_memcmp(want, mac, 32)) {
        status = PS_BAD_CODE;
        WARN("手机“%s”配对校验不过（二维码不是这次的？）", name);
        if (++pairing.bad >= PAIR_BAD_MAX) {
            WARN("配对校验失败太多次，关闭配对");
            pair_close("bad_code", NULL);
        }
        goto out;
    }
    /* 已绑定到上限时只允许“重新配对已有的手机”（同一个手机 ID、且是同一个用户，替换旧记录）。
     * 不能按全局 ID 放行：Paired 信号会把 ID 广播出去，别的 users 拿它顶掉自己的 5 台上限、还会把对方手机弄失效 */
    old = g_hash_table_lookup(phones, id);
    if (old && g_strcmp0(old->user, pairing.user))
        old = NULL;   /* 这台手机是别人的：当作新手机，照常受上限约束 */
    if (!old && user_phone_count(pairing.user) >= PHONES_PER_USER) {
        status = PS_TOO_MANY;
        WARN("用户 %s 已绑定 %d 台手机（上限），拒绝新手机“%s”", pairing.user, PHONES_PER_USER, name);
        goto out;
    }
    p = g_new0(Phone, 1);
    p->id = g_strdup(id);
    p->user = g_strdup(pairing.user);
    p->name = g_strdup(name);
    if (old)
        p->alias = g_strdup(old->alias);   /* 重新配对保留用户设的别名，别因为换密钥就丢了 */
    p->pub = g_bytes_new(pub, publen);
    p->paired = g_get_real_time() / G_USEC_PER_SEC;
    m = g_byte_array_new();
    put0(m, "hisi-phone-secret-v1");
    put0(m, pc_id);
    put(m, id, strlen(id));
    hmac(pairing.code, sizeof pairing.code, m, p->secret);
    g_byte_array_unref(m);
    m = g_byte_array_new();
    put0(m, "hisi-phone-pair-ok-v1");
    put(m, t->data, t->len);
    hmac(pairing.code, sizeof pairing.code, m, mac2);
    g_byte_array_unref(m);
    g_hash_table_replace(phones, p->id, p);
    store_save();
    INFO("已配对手机“%s”（%s），用户 %s", p->name, p->id, p->user);
    g_dbus_connection_emit_signal(bus, NULL, UI_PATH, ADMIN_IFACE, "Paired",
                                  g_variant_new("(sss)", p->id, p->user, p_disp(p)), NULL);
    phones_changed();
    /* 配对码只用一次 */
    pair_close("paired", p_disp(p));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i@ay)", PS_OK,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, mac2, 32, 1)));
    g_byte_array_unref(t);
    g_variant_unref(vpub);
    g_variant_unref(vmac);
    return;
out:
    if (t)
        g_byte_array_unref(t);
    g_variant_unref(vpub);
    g_variant_unref(vmac);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i@ay)", status, empty_ay()));
}

static void m_hello(GVariant *params, GDBusMethodInvocation *inv)
{
    const char *id;
    GVariant *vpn, *box = NULL, *vcn = NULL;
    const guint8 *pn;
    gsize pnlen;
    gint64 now = g_get_monotonic_time();
    int status = PS_BAD_REQUEST;
    Phone *p;
    Chal *c;
    GByteArray *m;
    char *info;

    g_variant_get(params, "(&s@ay)", &id, &vpn);
    pn = ay(vpn, &pnlen);
    if (!valid_id(id) || pnlen != 16)
        goto out;
    /* 先查配对表再计数：没配对的请求不该占已配对手机的名额（否则随便谁都能把 HELLO 限流打满） */
    if (!(p = g_hash_table_lookup(phones, id))) {
        status = PS_UNKNOWN_PHONE;
        DBG("未配对的手机 %s", id);
        goto out;
    }
    if (now - hello_rate.window > 60 * G_USEC_PER_SEC) {
        hello_rate.window = now;
        hello_rate.count = 0;
    }
    if (++hello_rate.count > HELLO_PER_MIN) {
        status = PS_BUSY;
        goto out;
    }
    if (held(p)) {
        status = PS_LOCKED_OUT;
        goto out;
    }
    c = chal_slot(id);
    chal_drop(c);
    g_strlcpy(c->phone_id, id, sizeof c->phone_id);
    rand_bytes(c->cn, sizeof c->cn);
    memcpy(c->pn, pn, 16);
    m = g_byte_array_new();
    put0(m, "hisi-phone-session-v1");
    put(m, c->cn, sizeof c->cn);
    put(m, c->pn, sizeof c->pn);
    hmac(p->secret, sizeof p->secret, m, c->ks);
    g_byte_array_unref(m);
    c->expires = now + CHAL_TTL_US;
    info = info_text(p);
    box = seal(c->ks, "hisi-info-v1", info, strlen(info));
    g_free(info);
    if (!box) {
        chal_drop(c);
        status = PS_ERROR;
        goto out;
    }
    vcn = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, c->cn, sizeof c->cn, 1);
    status = PS_OK;
    DBG("手机“%s”HELLO", p->name);
out:
    g_variant_unref(vpn);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i@ay@ay)", status, vcn ? vcn : empty_ay(),
                                                             box ? box : empty_ay()));
}

static void m_shot(GVariant *params, GDBusMethodInvocation *inv)
{
    const char *id;
    GVariant *vcn, *vmac;
    const guint8 *mac;
    gsize maclen;
    guint32 width;
    int status = PS_BAD_REQUEST;
    gboolean has, locked;
    gint64 secs;
    char wstr[16];
    guint8 want[32];
    GByteArray *m;
    Phone *p;
    Chal *c;

    g_variant_get(params, "(&s@ayu@ay)", &id, &vcn, &width, &vmac);
    mac = ay(vmac, &maclen);
    if (!valid_id(id) || maclen != 32)
        goto out;
    if (!(c = chal_find(id, vcn))) {
        status = PS_BAD_CHALLENGE;
        goto out;
    }
    if (!(p = g_hash_table_lookup(phones, id))) {
        status = PS_UNKNOWN_PHONE;
        goto out;
    }
    snprintf(wstr, sizeof wstr, "%u", width);
    m = g_byte_array_new();
    put0(m, "hisi-phone-shot-v1");
    put(m, wstr, strlen(wstr));
    hmac(c->ks, sizeof c->ks, m, want);
    g_byte_array_unref(m);
    if (CRYPTO_memcmp(want, mac, 32)) {
        status = PS_BAD_SIGNATURE;
        goto out;
    }
    bio_session_info(p->user, &has, &locked, &secs);
    if (!locked) {
        status = PS_NOT_LOCKED;
        goto out;
    }
    status = shot_start(inv, p->user, c->ks, width);
    if (status == PS_OK) {
        g_variant_unref(vcn);
        g_variant_unref(vmac);
        return;   /* shot_done 回复 */
    }
out:
    g_variant_unref(vcn);
    g_variant_unref(vmac);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i@ay)", status, empty_ay()));
}

static void m_unlock(GVariant *params, GDBusMethodInvocation *inv)
{
    const char *id;
    GVariant *vcn, *vsig;
    const guint8 *sig;
    gsize siglen;
    int status = PS_BAD_REQUEST;
    GByteArray *u;
    Phone *p;
    Chal *c;
    gboolean ok;

    g_variant_get(params, "(&s@ay@ay)", &id, &vcn, &vsig);
    sig = ay(vsig, &siglen);
    if (!valid_id(id) || !siglen || siglen > 160)
        goto out;
    if (!(c = chal_find(id, vcn))) {
        status = PS_BAD_CHALLENGE;
        goto out;
    }
    u = g_byte_array_new();
    put0(u, "hisi-phone-unlock-v1");
    put0(u, pc_id);
    put0(u, id);
    put(u, c->cn, sizeof c->cn);
    put(u, c->pn, sizeof c->pn);
    chal_drop(c);   /* 不管成败都作废 */
    if (!(p = g_hash_table_lookup(phones, id))) {
        g_byte_array_unref(u);
        status = PS_UNKNOWN_PHONE;
        goto out;
    }
    if (held(p)) {
        g_byte_array_unref(u);
        status = PS_LOCKED_OUT;
        goto out;
    }
    ok = verify(p->pub, u, sig, siglen);
    g_byte_array_unref(u);
    if (!ok) {
        status = PS_BAD_SIGNATURE;
        WARN("手机“%s”的解锁签名不对", p->name);
        if (++p->fails >= FAIL_MAX) {
            p->fails = 0;
            p->hold_until = g_get_monotonic_time() + FAIL_HOLD_US;
            WARN("手机“%s”签名连错 %d 次，暂停 %d 秒", p->name, FAIL_MAX, (int)(FAIL_HOLD_US / G_USEC_PER_SEC));
        }
        goto out;
    }
    p->fails = 0;
    {
        char *how = g_strdup_printf("手机“%s”指纹验证通过", p_disp(p));
        status = bio_unlock_user(p->user, how);
        g_free(how);
    }
out:
    g_variant_unref(vcn);
    g_variant_unref(vsig);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i)", status));
}

/* LOCK：mac = HMAC(ks, "hisi-phone-lock-v1")，不要指纹；不管成败 cn 作废 */
static void m_lock(GVariant *params, GDBusMethodInvocation *inv)
{
    static const char label[] = "hisi-phone-lock-v1";
    const char *id;
    GVariant *vcn, *vmac;
    const guint8 *mac;
    gsize maclen;
    int status = PS_BAD_REQUEST;
    guint8 want[32];
    GByteArray *m;
    Phone *p;
    Chal *c;
    char *how;

    g_variant_get(params, "(&s@ay@ay)", &id, &vcn, &vmac);
    mac = ay(vmac, &maclen);
    if (!valid_id(id) || maclen != 32)
        goto out;
    if (!(c = chal_find(id, vcn))) {
        status = PS_BAD_CHALLENGE;
        goto out;
    }
    m = g_byte_array_new();
    put(m, label, strlen(label));
    hmac(c->ks, sizeof c->ks, m, want);
    g_byte_array_unref(m);
    chal_drop(c);
    if (!(p = g_hash_table_lookup(phones, id))) {
        status = PS_UNKNOWN_PHONE;
        goto out;
    }
    if (CRYPTO_memcmp(want, mac, 32)) {
        status = PS_BAD_SIGNATURE;
        WARN("手机“%s”的锁屏请求校验不对", p->name);
        goto out;
    }
    how = g_strdup_printf("手机“%s”要求锁屏", p_disp(p));
    status = bio_lock_user(p->user, how);
    g_free(how);
out:
    g_variant_unref(vcn);
    g_variant_unref(vmac);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i)", status));
}

static gboolean away_fire(gpointer data)
{
    Phone *p = data;
    char *how;
    int r;

    p->away_timer = 0;
    p->away = 0;   /* 锁一次就算关了，手机连回来重新开 */
    how = g_strdup_printf("手机“%s”离开（蓝牙断开后没连回来）", p_disp(p));
    r = bio_lock_user(p->user, how);
    g_free(how);
    if (r != PS_OK)
        WARN("离开自动锁屏没锁成（状态 %d）", r);
    return G_SOURCE_REMOVE;
}

static void away_cancel(Phone *p)
{
    if (p->away_timer) {
        g_source_remove(p->away_timer);
        p->away_timer = 0;
        INFO("手机“%s”连回来了，取消离开锁屏", p->name);
    }
}

/* AWAY：grace 0 = 关，5～600 = 开；mac = HMAC(ks, "hisi-phone-away-v1" ‖ 0 ‖ grace 十进制)。cn 不作废。
 * 只收蓝牙会话发来的（hisi-phone-link 把关），重新连上后再发一次就取消正在进行的倒计时 */
static void m_away(GVariant *params, GDBusMethodInvocation *inv)
{
    const char *id;
    GVariant *vcn, *vmac;
    const guint8 *mac;
    gsize maclen;
    guint32 grace;
    int status = PS_BAD_REQUEST;
    char gstr[16];
    guint8 want[32];
    GByteArray *m;
    Phone *p;
    Chal *c;

    g_variant_get(params, "(&s@ayu@ay)", &id, &vcn, &grace, &vmac);
    mac = ay(vmac, &maclen);
    if (!valid_id(id) || maclen != 32 || (grace && (grace < AWAY_MIN_S || grace > AWAY_MAX_S)))
        goto out;
    if (!(c = chal_find(id, vcn))) {
        status = PS_BAD_CHALLENGE;
        goto out;
    }
    if (!(p = g_hash_table_lookup(phones, id))) {
        status = PS_UNKNOWN_PHONE;
        goto out;
    }
    snprintf(gstr, sizeof gstr, "%u", grace);
    m = g_byte_array_new();
    put0(m, "hisi-phone-away-v1");
    put(m, gstr, strlen(gstr));
    hmac(c->ks, sizeof c->ks, m, want);
    g_byte_array_unref(m);
    if (CRYPTO_memcmp(want, mac, 32)) {
        status = PS_BAD_SIGNATURE;
        WARN("手机“%s”的离开锁屏设置校验不对", p->name);
        goto out;
    }
    away_cancel(p);
    if (p->away != grace) {
        if (grace)
            INFO("手机“%s”开启离开自动锁屏：蓝牙断开 %u 秒没连回来就锁屏", p->name, grace);
        else
            INFO("手机“%s”关闭离开自动锁屏", p->name);
    }
    p->away = grace;
    status = PS_OK;
out:
    g_variant_unref(vcn);
    g_variant_unref(vmac);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i)", status));
}

/* hisi-phone-link 报告：发过 AWAY 的那条蓝牙连接断开了。开着离开锁屏就开始倒计时 */
static void m_gone(GVariant *params, GDBusMethodInvocation *inv)
{
    const char *id;
    Phone *p;

    g_variant_get(params, "(&s)", &id);
    p = valid_id(id) ? g_hash_table_lookup(phones, id) : NULL;
    if (p && p->away && !p->away_timer) {
        INFO("手机“%s”蓝牙断开，%u 秒内没连回来就锁屏", p->name, p->away);
        p->away_timer = g_timeout_add_seconds(p->away, away_fire, p);
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i)", p ? PS_OK : PS_UNKNOWN_PHONE));
}

/* 【手机在线】hisi-phone-link 报告哪些已配对的手机现在连着电脑（蓝牙连接或 Wi-Fi 会话里 HELLO 过），
 * 只在内存里；锁屏界面据此显示“手机已连接”。HELLO 不证明手机身份（真正的验证在解锁时），所以这只是界面提示，
 * 不影响解锁。hisi-phone-link 重启或不在了，会话都断了，下次连上会重新报。 */

static void m_presence(GVariant *params, GDBusMethodInvocation *inv)
{
    const char *id;
    gboolean on;
    Phone *p;

    g_variant_get(params, "(&sb)", &id, &on);
    p = valid_id(id) ? g_hash_table_lookup(phones, id) : NULL;
    if (p) {
        gint64 *since = g_hash_table_lookup(online, id);

        if (on && !since) {
            since = g_new(gint64, 1);
            *since = g_get_monotonic_time();
            g_hash_table_insert(online, g_strdup(id), since);
            INFO("手机“%s”已连接", p->name);
            bio_ui_refresh();
            phones_changed();
        } else if (!on && since) {
            g_hash_table_remove(online, id);
            INFO("手机“%s”已断开", p->name);
            bio_ui_refresh();
            phones_changed();
        }
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(i)", p ? PS_OK : PS_UNKNOWN_PHONE));
}

const char *phone_online_name(const char *user, guint *count)
{
    GHashTableIter it;
    gpointer k, v;
    const char *best = NULL;
    gint64 best_since = 0;

    *count = 0;
    g_hash_table_iter_init(&it, online);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Phone *p = g_hash_table_lookup(phones, k);

        if (!p || g_strcmp0(p->user, user))
            continue;
        (*count)++;
        if (!best || *(gint64 *)v > best_since) {
            best = p_disp(p);
            best_since = *(gint64 *)v;
        }
    }
    return best;
}

/* 【配对和管理：Admin 接口（root，sudo hisi-phone …）和 User 接口（桌面用户自己，控制中心插件）共用】 */

#define POLKIT_TIMEOUT_MS (5 * 60 * 1000)   /* 等用户在认证框里输密码 */

/* 只说“变了”、不带内容，谁都收得到；客户端再用 User.List 只看自己的 */
static void phones_changed(void)
{
    g_dbus_connection_emit_signal(bus, NULL, UI_PATH, USER_IFACE, "Changed", NULL, NULL);
}

/* 关闭配对窗口，配对码作废。result：paired / timeout / cancelled / replaced / bad_code。
 * 经控制中心开的配对，结果单播给开配对的那个客户端（PairEnded），别人收不到 */
static void pair_close(const char *result, const char *name)
{
    if (pairing.timer) {
        g_source_remove(pairing.timer);
        pairing.timer = 0;
    }
    pairing.open = FALSE;
    OPENSSL_cleanse(pairing.code, sizeof pairing.code);
    if (pairing.owner_watch) {
        g_bus_unwatch_name(pairing.owner_watch);
        pairing.owner_watch = 0;
    }
    if (pairing.owner) {
        g_dbus_connection_emit_signal(bus, pairing.owner, UI_PATH, USER_IFACE, "PairEnded",
                                      g_variant_new("(ss)", result, name ? name : ""), NULL);
        g_clear_pointer(&pairing.owner, g_free);
    }
}

static gboolean pair_timeout(gpointer data)
{
    pairing.timer = 0;
    if (pairing.open)
        INFO("配对超时，已关闭");
    pair_close("timeout", NULL);
    return G_SOURCE_REMOVE;
}

static void owner_vanished(GDBusConnection *c, const char *name, gpointer data)
{
    if (pairing.open && !g_strcmp0(pairing.owner, name)) {
        INFO("开配对的控制中心退出了，取消配对");
        pair_close("cancelled", NULL);
    }
}

/* 能配手机、管手机的“桌面用户”：普通用户 uid，排除 greeter 用户 dde 和 hisi-phone-link 的 hisi-phone */
static gboolean desktop_user(const struct passwd *pw)
{
    return pw && pw->pw_uid >= 1000 && pw->pw_uid < 60000 && strcmp(pw->pw_name, "dde")
        && strcmp(pw->pw_name, LINK_USER);
}

/* 开配对窗口（顶掉没完成的），回 (电脑 ID, 配对码, 主机名)。owner 非 NULL 时那个客户端退出就取消 */
static void pair_open(const char *user, guint32 secs, const char *owner, GDBusMethodInvocation *inv)
{
    char host[256] = "";

    if (pairing.open)
        INFO("新的配对顶掉了还没完成的配对");
    pair_close("replaced", NULL);
    g_free(pairing.user);
    pairing.user = g_strdup(user);
    rand_bytes(pairing.code, sizeof pairing.code);
    pairing.bad = 0;
    pairing.open = TRUE;
    secs = CLAMP(secs, PAIR_MIN_S, PAIR_MAX_S);
    pairing.timer = g_timeout_add_seconds(secs, pair_timeout, NULL);
    if (owner) {
        pairing.owner = g_strdup(owner);
        pairing.owner_watch = g_bus_watch_name_on_connection(bus, owner, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                             NULL, owner_vanished, NULL, NULL);
    }
    gethostname(host, sizeof host - 1);
    INFO("开始配对（用户 %s，%u 秒%s）", user, secs, owner ? "，在控制中心" : "");
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s@ays)", pc_id,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pairing.code, sizeof pairing.code, 1), host));
}

static void m_pair_start(GVariant *params, GDBusMethodInvocation *inv)
{
    const char *user;
    guint32 secs;

    g_variant_get(params, "(&su)", &user, &secs);
    if (!desktop_user(getpwnam(user))) {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "不是桌面用户：%s", user);
        return;
    }
    pair_open(user, secs, NULL, inv);
}

static void m_list(GDBusMethodInvocation *inv)
{
    GVariantBuilder b;
    GHashTableIter it;
    Phone *p;

    g_variant_builder_init(&b, G_VARIANT_TYPE("a(ssst)"));
    g_hash_table_iter_init(&it, phones);
    while (g_hash_table_iter_next(&it, NULL, (gpointer *)&p))
        g_variant_builder_add(&b, "(ssst)", p->id, p->user, p_disp(p), (guint64)p->paired);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ssst))", &b));
}

/* User.List：只列调用者自己的手机，带在线状态和每用户上限 */
static void m_user_list(const char *user, GDBusMethodInvocation *inv)
{
    GVariantBuilder b;
    GHashTableIter it;
    Phone *p;

    g_variant_builder_init(&b, G_VARIANT_TYPE("a(sstb)"));
    g_hash_table_iter_init(&it, phones);
    while (g_hash_table_iter_next(&it, NULL, (gpointer *)&p))
        if (!g_strcmp0(p->user, user))
            g_variant_builder_add(&b, "(sstb)", p->id, p_disp(p), (guint64)p->paired,
                                  g_hash_table_contains(online, p->id));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(a(sstb)u)", &b, (guint32)PHONES_PER_USER));
}

/* only_user 非 NULL（User 接口）时只能删这个用户自己的手机，别人的当作没有 */
static void m_unpair(GVariant *params, GDBusMethodInvocation *inv, const char *only_user)
{
    const char *id;
    Phone *p;
    gboolean removed = FALSE;

    g_variant_get(params, "(&s)", &id);
    p = g_hash_table_lookup(phones, id);
    if (p && (!only_user || !strcmp(p->user, only_user))) {
        INFO("已删除手机“%s”（%s）%s", p_disp(p), id, only_user ? "，在控制中心删的" : "");
        g_hash_table_remove(phones, id);
        removed = TRUE;
        for (int i = 0; i < MAX_CHALS; i++)
            if (!strcmp(chals[i].phone_id, id))
                chal_drop(&chals[i]);
        if (g_hash_table_remove(online, id))
            bio_ui_refresh();
        store_save();
        phones_changed();
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", removed));
}

/* 设置别名（锁屏、列表、手机 App 里显示的这台电脑列表用不到它——只影响电脑这边显示什么）。
 * 别名不动配对时手机上报的名字（name 永远保留），空名 = 清除别名恢复原名。
 * 配对密钥、密钥保护方式都不变，所以不用重新配对。only_user 同 m_unpair */
static void m_rename(GVariant *params, GDBusMethodInvocation *inv, const char *only_user)
{
    const char *id, *name;
    Phone *p;

    g_variant_get(params, "(&s&s)", &id, &name);
    if (strlen(name) > NAME_MAX_LEN) {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                              "最长 %d 字节", NAME_MAX_LEN);
        return;
    }
    for (const char *c = name; *c; c++) {
        if ((guchar)*c < 0x20 || (guchar)*c == 0x7f) {
            g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "别名里不能有控制字符");
            return;
        }
    }
    p = g_hash_table_lookup(phones, id);
    if (p && only_user && strcmp(p->user, only_user))
        p = NULL;
    if (p) {
        g_free(p->alias);
        p->alias = *name ? g_strdup(name) : NULL;
        if (p->alias)
            INFO("手机“%s”（%s）的别名设为“%s”", p->name, p->id, p->alias);
        else
            INFO("手机“%s”（%s）的别名清除了，恢复显示上报名", p->name, p->id);
        store_save();
        if (g_hash_table_contains(online, id))
            bio_ui_refresh();
        phones_changed();
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", p != NULL));
}

/* User.PairStart：先问 polkit（org.hisi.bio-unlock.pair-phone，默认要输当前用户的密码），过了再开配对窗口。
 * 配好的手机能解锁这个用户的会话，所以不能只凭“会话开着”就让配 */
typedef struct {
    GDBusMethodInvocation *inv;
    char *user;
    guint32 secs;
} PairAuth;

static void pair_auth_done(GObject *src, GAsyncResult *res, gpointer data)
{
    PairAuth *a = data;
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
    gboolean ok = FALSE, challenge = FALSE;

    if (r) {
        GVariant *details;

        g_variant_get(r, "((bb@a{ss}))", &ok, &challenge, &details);
        g_variant_unref(details);
        g_variant_unref(r);
    }
    if (e) {
        WARN("向 polkit 确认配对授权失败：%s", e->message);
        g_dbus_method_invocation_return_error(a->inv, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                              "没法确认授权：%s", e->message);
        g_error_free(e);
    } else if (!ok) {
        INFO("用户 %s 在控制中心配对手机，没通过认证", a->user);
        g_dbus_method_invocation_return_error(a->inv, G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED, "没有通过认证");
    } else {
        pair_open(a->user, a->secs, g_dbus_method_invocation_get_sender(a->inv), a->inv);
    }
    g_free(a->user);
    g_free(a);
}

static void user_pair_start(GVariant *params, GDBusMethodInvocation *inv, const char *user)
{
    GVariantBuilder subject, details;
    PairAuth *a = g_new0(PairAuth, 1);

    g_variant_get(params, "(u)", &a->secs);
    a->inv = inv;
    a->user = g_strdup(user);
    g_variant_builder_init(&subject, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&subject, "{sv}", "name", g_variant_new_string(g_dbus_method_invocation_get_sender(inv)));
    g_variant_builder_init(&details, G_VARIANT_TYPE("a{ss}"));
    g_dbus_connection_call(bus, "org.freedesktop.PolicyKit1", "/org/freedesktop/PolicyKit1/Authority",
                           "org.freedesktop.PolicyKit1.Authority", "CheckAuthorization",
                           g_variant_new("((sa{sv})sa{ss}us)", "system-bus-name", &subject, POLKIT_PAIR_ACTION,
                                         &details, (guint32)1 /* AllowUserInteraction */, ""),
                           G_VARIANT_TYPE("((bba{ss}))"), G_DBUS_CALL_FLAGS_NONE, POLKIT_TIMEOUT_MS, NULL,
                           pair_auth_done, a);
}

/* D-Bus 策略已经限定了谁能调哪个接口，这里再按 uid 核对一遍 */
static uid_t caller_uid(GDBusMethodInvocation *inv)
{
    GVariant *r;
    guint32 uid = (guint32)-1;

    r = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                    "GetConnectionUnixUser",
                                    g_variant_new("(s)", g_dbus_method_invocation_get_sender(inv)),
                                    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    if (r) {
        g_variant_get(r, "(u)", &uid);
        g_variant_unref(r);
    }
    return uid;
}

static void phone_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    uid_t uid = caller_uid(inv);

    if (uid != 0 && uid != link_uid) {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "只给 " LINK_USER);
        return;
    }
    if (!strcmp(method, "GetLinkInfo")) {
        char host[256] = "";
        gethostname(host, sizeof host - 1);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ss)", pc_id, host));
    } else if (!strcmp(method, "Pair")) {
        m_pair(params, inv);
    } else if (!strcmp(method, "Hello")) {
        m_hello(params, inv);
    } else if (!strcmp(method, "Shot")) {
        m_shot(params, inv);
    } else if (!strcmp(method, "Unlock")) {
        m_unlock(params, inv);
    } else if (!strcmp(method, "Lock")) {
        m_lock(params, inv);
    } else if (!strcmp(method, "Away")) {
        m_away(params, inv);
    } else if (!strcmp(method, "Gone")) {
        m_gone(params, inv);
    } else if (!strcmp(method, "Presence")) {
        m_presence(params, inv);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "没有方法 %s", method);
    }
}

static void admin_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    if (caller_uid(inv) != 0) {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "只给 root");
        return;
    }
    if (!strcmp(method, "PairStart")) {
        m_pair_start(params, inv);
    } else if (!strcmp(method, "PairStop")) {
        if (pairing.open)
            INFO("配对已取消");
        pair_close("cancelled", NULL);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "List")) {
        m_list(inv);
    } else if (!strcmp(method, "Unpair")) {
        m_unpair(params, inv, NULL);
    } else if (!strcmp(method, "Rename")) {
        m_rename(params, inv, NULL);
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "没有方法 %s", method);
    }
}

/* 【org.hisi.BioUnlock1.User：桌面用户自己（控制中心插件）】谁调就按谁的 uid 只管谁的手机 */
static void user_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    uid_t uid = caller_uid(inv);
    struct passwd *pw = uid == (uid_t)-1 ? NULL : getpwuid(uid);
    char *user;

    if (!desktop_user(pw)) {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED, "只给桌面用户");
        return;
    }
    user = g_strdup(pw->pw_name);   /* getpwuid 的缓冲区会被后面的 getpw* 覆盖 */
    if (!strcmp(method, "List")) {
        m_user_list(user, inv);
    } else if (!strcmp(method, "Rename")) {
        m_rename(params, inv, user);
    } else if (!strcmp(method, "Unpair")) {
        m_unpair(params, inv, user);
    } else if (!strcmp(method, "PairStart")) {
        user_pair_start(params, inv, user);
    } else if (!strcmp(method, "PairStop")) {
        /* 只能取消自己（这个客户端）开的配对 */
        if (pairing.open && !g_strcmp0(pairing.owner, sender)) {
            INFO("控制中心取消了配对");
            pair_close("cancelled", NULL);
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "Lock")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(i)",
            bio_lock_user(user, "控制中心里点了“锁屏试试”")));
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "没有方法 %s", method);
    }
    g_free(user);
}

static const char phone_xml[] =
    "<node>"
    " <interface name='" PHONE_IFACE "'>"
    "  <method name='GetLinkInfo'>"
    "   <arg type='s' name='pc_id' direction='out'/>"
    "   <arg type='s' name='host' direction='out'/>"
    "  </method>"
    "  <method name='Pair'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='s' name='name' direction='in'/>"
    "   <arg type='ay' name='pub' direction='in'/>"
    "   <arg type='ay' name='mac' direction='in'/>"
    "   <arg type='i' name='status' direction='out'/>"
    "   <arg type='ay' name='mac2' direction='out'/>"
    "  </method>"
    "  <method name='Hello'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='ay' name='pn' direction='in'/>"
    "   <arg type='i' name='status' direction='out'/>"
    "   <arg type='ay' name='cn' direction='out'/>"
    "   <arg type='ay' name='box' direction='out'/>"
    "  </method>"
    "  <method name='Shot'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='ay' name='cn' direction='in'/>"
    "   <arg type='u' name='width' direction='in'/>"
    "   <arg type='ay' name='mac' direction='in'/>"
    "   <arg type='i' name='status' direction='out'/>"
    "   <arg type='ay' name='box' direction='out'/>"
    "  </method>"
    "  <method name='Unlock'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='ay' name='cn' direction='in'/>"
    "   <arg type='ay' name='sig' direction='in'/>"
    "   <arg type='i' name='status' direction='out'/>"
    "  </method>"
    "  <method name='Lock'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='ay' name='cn' direction='in'/>"
    "   <arg type='ay' name='mac' direction='in'/>"
    "   <arg type='i' name='status' direction='out'/>"
    "  </method>"
    "  <method name='Away'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='ay' name='cn' direction='in'/>"
    "   <arg type='u' name='grace' direction='in'/>"
    "   <arg type='ay' name='mac' direction='in'/>"
    "   <arg type='i' name='status' direction='out'/>"
    "  </method>"
    "  <method name='Gone'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='i' name='status' direction='out'/>"
    "  </method>"
    "  <method name='Presence'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='b' name='online' direction='in'/>"
    "   <arg type='i' name='status' direction='out'/>"
    "  </method>"
    " </interface>"
    " <interface name='" ADMIN_IFACE "'>"
    "  <method name='PairStart'>"
    "   <arg type='s' name='user' direction='in'/>"
    "   <arg type='u' name='seconds' direction='in'/>"
    "   <arg type='s' name='pc_id' direction='out'/>"
    "   <arg type='ay' name='code' direction='out'/>"
    "   <arg type='s' name='host' direction='out'/>"
    "  </method>"
    "  <method name='PairStop'/>"
    "  <method name='List'>"
    "   <arg type='a(ssst)' name='phones' direction='out'/>"
    "  </method>"
    "  <method name='Unpair'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='b' name='removed' direction='out'/>"
    "  </method>"
    "  <method name='Rename'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='s' name='alias' direction='in'/>"
    "   <arg type='b' name='renamed' direction='out'/>"
    "  </method>"
    "  <signal name='Paired'>"
    "   <arg type='s' name='phone_id'/>"
    "   <arg type='s' name='user'/>"
    "   <arg type='s' name='name'/>"
    "  </signal>"
    " </interface>"
    " <interface name='" USER_IFACE "'>"
    "  <method name='List'>"
    "   <arg type='a(sstb)' name='phones' direction='out'/>"
    "   <arg type='u' name='max_phones' direction='out'/>"
    "  </method>"
    "  <method name='Rename'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='s' name='alias' direction='in'/>"
    "   <arg type='b' name='renamed' direction='out'/>"
    "  </method>"
    "  <method name='Unpair'>"
    "   <arg type='s' name='phone_id' direction='in'/>"
    "   <arg type='b' name='removed' direction='out'/>"
    "  </method>"
    "  <method name='PairStart'>"
    "   <arg type='u' name='seconds' direction='in'/>"
    "   <arg type='s' name='pc_id' direction='out'/>"
    "   <arg type='ay' name='code' direction='out'/>"
    "   <arg type='s' name='host' direction='out'/>"
    "  </method>"
    "  <method name='PairStop'/>"
    "  <method name='Lock'>"
    "   <arg type='i' name='status' direction='out'/>"
    "  </method>"
    "  <signal name='Changed'/>"
    "  <signal name='PairEnded'>"
    "   <arg type='s' name='result'/>"
    "   <arg type='s' name='name'/>"
    "  </signal>"
    " </interface>"
    "</node>";

void phone_init(void)
{
    static const GDBusInterfaceVTable phone_vt = { phone_method, NULL, NULL };
    static const GDBusInterfaceVTable admin_vt = { admin_method, NULL, NULL };
    static const GDBusInterfaceVTable user_vt = { user_method, NULL, NULL };
    GError *e = NULL;
    GDBusNodeInfo *node;
    struct passwd *pw;

    umask(077);
    if (g_mkdir_with_parents(PHONE_STATE_DIR, 0700))
        WARN("建 %s 失败：%s", PHONE_STATE_DIR, g_strerror(errno));
    phones = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, phone_free);
    online = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    store_load();
    if (!pc_id[0]) {
        guint8 raw[16];
        rand_bytes(raw, sizeof raw);
        to_hex(raw, sizeof raw, pc_id);
        store_save();
        INFO("生成电脑 ID %s", pc_id);
    }
    if ((pw = getpwnam(LINK_USER)))
        link_uid = pw->pw_uid;
    node = g_dbus_node_info_new_for_xml(phone_xml, &e);
    if (!node
        || !g_dbus_connection_register_object(bus, UI_PATH, node->interfaces[0], &phone_vt, NULL, NULL, &e)
        || !g_dbus_connection_register_object(bus, UI_PATH, node->interfaces[1], &admin_vt, NULL, NULL, &e)
        || !g_dbus_connection_register_object(bus, UI_PATH, node->interfaces[2], &user_vt, NULL, NULL, &e)) {
        WARN("导出手机解锁接口失败：%s", e->message);
        g_error_free(e);
    }
    if (node)
        g_dbus_node_info_unref(node);
    INFO("手机解锁：已配对 %u 台手机%s", g_hash_table_size(phones),
         link_uid == (uid_t)-1 ? "（没有用户 " LINK_USER "，hisi-phone-link 调不进来）" : "");
}
