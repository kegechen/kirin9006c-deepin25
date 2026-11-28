/*
 * hisi-bio-unlock phone ...（一般经 /usr/local/sbin/hisi-phone 调用，root）：手机指纹解锁的配对与管理。
 * 只是客户端：经 org.hisi.BioUnlock1.Admin 找正在运行的 hisi-bio-unlock 服务。
 *   pair [用户名]     显示配对二维码（3 分钟有效，用一次作废），手机“电脑解锁”App 扫码；默认配给 sudo 前的用户
 *   list              列出已配对的手机
 *   unpair <ID 前缀>  删除一台手机
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <glib-unix.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "hisi-bio.h"

#define LINK_PORT    47390
#define PAIR_SECONDS 180

static GMainLoop *ploop;
static int pair_rc = 1;
static const char *pair_user;

static GVariant *admin(const char *method, GVariant *args, const char *ret, GError **e)
{
    return g_dbus_connection_call_sync(bus, UI_SVC, UI_PATH, ADMIN_IFACE, method, args,
                                       ret ? G_VARIANT_TYPE(ret) : NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, e);
}

static void admin_fail(const char *what, GError *e)
{
    fprintf(stderr, "%s失败：%s\n", what, e->message);
    if (g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
        || g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)
        || g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE))
        fprintf(stderr, "hisi-bio-unlock.service 没运行或版本太旧：sudo systemctl restart hisi-bio-unlock\n");
    g_error_free(e);
}

/* 本机的 IPv4 地址，逗号分隔 */
static char *local_ipv4s(void)
{
    struct ifaddrs *ifa, *i;
    GString *s = g_string_new(NULL);

    if (getifaddrs(&ifa) == 0) {
        for (i = ifa; i; i = i->ifa_next) {
            char buf[INET_ADDRSTRLEN];

            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)
                || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING))
                continue;
            if (!inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, buf, sizeof buf))
                continue;
            if (s->len)
                g_string_append_c(s, ',');
            g_string_append(s, buf);
        }
        freeifaddrs(ifa);
    }
    return g_string_free(s, FALSE);
}

/* 已上电的蓝牙适配器地址，没有返回空串 */
static char *bt_address(void)
{
    GVariant *r, *ifaces;
    GVariantIter *it;
    const char *path;
    char *addr = NULL;

    r = g_dbus_connection_call_sync(bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                                    NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    if (!r)
        return g_strdup("");
    g_variant_get(r, "(a{oa{sa{sv}}})", &it);
    while (!addr && g_variant_iter_next(it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
        gboolean powered = FALSE;

        if (props) {
            g_variant_lookup(props, "Powered", "b", &powered);
            if (powered)
                g_variant_lookup(props, "Address", "s", &addr);
            g_variant_unref(props);
        }
        g_variant_unref(ifaces);
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    return addr ? addr : g_strdup("");
}

static void show_qr(const char *uri)
{
    char *qr = g_find_program_in_path("qrencode"), *out = NULL;
    const char *argv[] = { qr, "-t", "ANSIUTF8", "-m", "2", uri, NULL };
    gint status = -1;

    if (qr && g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &status, NULL)
        && g_spawn_check_wait_status(status, NULL)) {
        fputs(out, stdout);
    } else {
        printf("（没装 qrencode，显示不了二维码：sudo apt install qrencode。也可以把下面的链接发到手机上，在 App 里“粘贴配对链接”）\n");
    }
    g_free(out);
    g_free(qr);
}

static gboolean link_running(void)
{
    const char *argv[] = { "systemctl", "is-active", "--quiet", "hisi-phone-link.service", NULL };
    gint status = -1;

    return g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                        | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL)
        && g_spawn_check_wait_status(status, NULL);
}

static void on_paired(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *member, GVariant *params, gpointer data)
{
    const char *id, *user, *name;

    g_variant_get(params, "(&s&s&s)", &id, &user, &name);
    if (g_strcmp0(user, pair_user))
        return;
    printf("\n已配对：手机“%s”（ID %.8s…），用户 %s。锁屏后在手机上打开“电脑解锁”，按指纹即可解锁。\n", name, id, user);
    pair_rc = 0;
    g_main_loop_quit(ploop);
}

static gboolean on_pair_timeout(gpointer data)
{
    printf("\n配对超时（%d 秒），二维码已作废。\n", PAIR_SECONDS);
    g_main_loop_quit(ploop);
    return G_SOURCE_REMOVE;
}

static gboolean on_pair_signal(gpointer data)
{
    printf("\n已取消配对。\n");
    pair_rc = 130;
    g_main_loop_quit(ploop);
    return G_SOURCE_REMOVE;
}

static char *b64url(const guint8 *d, gsize n)
{
    char *s = g_base64_encode(d, n), *p;

    for (p = s; *p; p++) {
        if (*p == '+')
            *p = '-';
        else if (*p == '/')
            *p = '_';
        else if (*p == '=')
            *p = 0;
    }
    return s;
}

/* 该用户已绑定的手机数；读不到返回 0 */
static int paired_count(const char *user)
{
    GVariant *r = admin("List", NULL, "(a(ssst))", NULL);
    GVariantIter *it;
    const char *id, *u, *name;
    guint64 paired;
    int n = 0;

    if (!r)
        return 0;
    g_variant_get(r, "(a(ssst))", &it);
    while (g_variant_iter_next(it, "(&s&s&st)", &id, &u, &name, &paired))
        if (!g_strcmp0(u, user))
            n++;
    g_variant_iter_free(it);
    g_variant_unref(r);
    return n;
}

static int cmd_pair(const char *user)
{
    GError *e = NULL;
    GVariant *r, *vcode;
    const char *pcid, *host;
    const guint8 *code;
    gsize n;
    char *code64, *ips, *bt, *h, *u, *uri;

    r = admin("PairStart", g_variant_new("(su)", user, PAIR_SECONDS), "(says)", &e);
    if (!r) {
        admin_fail("开始配对", e);
        return 1;
    }
    g_variant_get(r, "(&s@ay&s)", &pcid, &vcode, &host);
    code = g_variant_get_fixed_array(vcode, &n, 1);
    code64 = b64url(code, n);
    ips = local_ipv4s();
    bt = bt_address();
    h = g_uri_escape_string(host, NULL, FALSE);
    u = g_uri_escape_string(user, NULL, FALSE);
    uri = g_strdup_printf("hisiphone://pair?v=1&id=%s&c=%s&h=%s&u=%s&ip=%s&p=%d&bt=%s",
                          pcid, code64, h, u, ips, LINK_PORT, bt);
    pair_user = user;
    g_dbus_connection_signal_subscribe(bus, UI_SVC, ADMIN_IFACE, "Paired", UI_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_paired, NULL, NULL);

    printf("给用户 %s 配对手机。手机打开“电脑解锁”App →“配对新电脑”→ 扫下面的二维码（%d 秒内有效，用一次作废）：\n\n",
           user, PAIR_SECONDS);
    show_qr(uri);
    printf("\n配对链接（等同二维码，别发给别人）：\n%s\n\n", uri);
    printf("电脑：%s　IP：%s　蓝牙：%s\n", host, *ips ? ips : "（无）", *bt ? bt : "（没开）");
    if (!*ips && !*bt)
        printf("提示：没有网络也没开蓝牙，手机连不上。\n");
    if (!link_running())
        printf("提示：hisi-phone-link.service 没在运行，手机连不上：sudo systemctl enable --now hisi-phone-link\n");
    if (paired_count(user) >= PHONES_PER_USER)
        printf("提示：用户 %s 已绑定 %d 台手机（上限）。只能重新配对已有的手机；新手机请先删除一台：sudo hisi-phone unpair <ID>\n",
               user, PHONES_PER_USER);
    printf("等待手机……（Ctrl+C 取消）\n");
    fflush(stdout);

    ploop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(PAIR_SECONDS, on_pair_timeout, NULL);
    g_unix_signal_add(SIGINT, on_pair_signal, NULL);
    g_unix_signal_add(SIGTERM, on_pair_signal, NULL);
    g_main_loop_run(ploop);
    if (pair_rc) {
        GVariant *s = admin("PairStop", NULL, NULL, NULL);
        if (s)
            g_variant_unref(s);
    }
    memset(code64, 0, strlen(code64));
    memset(uri, 0, strlen(uri));
    g_free(code64);
    g_free(uri);
    g_free(ips);
    g_free(bt);
    g_free(h);
    g_free(u);
    g_variant_unref(vcode);
    g_variant_unref(r);
    return pair_rc;
}

static int cmd_list(void)
{
    GError *e = NULL;
    GVariant *r;
    GVariantIter *it;
    const char *id, *user, *name;
    guint64 paired;
    int count = 0;

    r = admin("List", NULL, "(a(ssst))", &e);
    if (!r) {
        admin_fail("读取", e);
        return 1;
    }
    g_variant_get(r, "(a(ssst))", &it);
    while (g_variant_iter_next(it, "(&s&s&st)", &id, &user, &name, &paired)) {
        char when[32] = "";
        time_t t = (time_t)paired;
        struct tm tm;

        if (!count++)
            printf("%-34s %-12s %-20s %s\n", "ID", "用户", "手机", "配对时间");
        if (localtime_r(&t, &tm))
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
        printf("%-34s %-12s %-20s %s\n", id, user, name, when);
    }
    if (!count)
        printf("还没有配对的手机。配对：sudo hisi-phone pair\n");
    g_variant_iter_free(it);
    g_variant_unref(r);
    return 0;
}

/* 按 ID 前缀找唯一一台手机，返回 "ID\t名字"（g_free）；找不到或不唯一时打印原因返回 NULL。*status 是退出码 */
static char *find_phone(const char *prefix, int *status)
{
    GError *e = NULL;
    GVariant *r;
    GVariantIter *it;
    const char *id, *user, *name;
    guint64 paired;
    char *match = NULL;
    int hits = 0;

    if (strlen(prefix) < 4) {
        fprintf(stderr, "ID 至少给前 4 位（sudo hisi-phone list 查看）\n");
        *status = 2;
        return NULL;
    }
    r = admin("List", NULL, "(a(ssst))", &e);
    if (!r) {
        admin_fail("读取", e);
        *status = 1;
        return NULL;
    }
    g_variant_get(r, "(a(ssst))", &it);
    while (g_variant_iter_next(it, "(&s&s&st)", &id, &user, &name, &paired)) {
        if (g_str_has_prefix(id, prefix)) {
            hits++;
            g_free(match);
            match = g_strdup_printf("%s\t%s", id, name);
        }
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    if (hits != 1) {
        fprintf(stderr, hits ? "前缀 %s 对上了多台手机，多给几位\n" : "没有 ID 以 %s 开头的手机\n", prefix);
        g_free(match);
        *status = 1;
        return NULL;
    }
    return match;
}

static int cmd_rename(const char *prefix, const char *newname)
{
    GError *e = NULL;
    GVariant *d;
    char *match;
    gboolean ok = FALSE;
    int status = 1;

    if (!g_utf8_validate(newname, -1, NULL)) {
        fprintf(stderr, "别名不是有效的 UTF-8\n");
        return 2;
    }
    match = find_phone(prefix, &status);
    if (!match)
        return status;
    *strchr(match, '\t') = 0;
    d = admin("Rename", g_variant_new("(ss)", match, newname), "(b)", &e);
    if (!d) {
        admin_fail("设别名", e);
        g_free(match);
        return 1;
    }
    g_variant_get(d, "(b)", &ok);
    g_variant_unref(d);
    if (ok)
        printf("已把“%s”的别名设为“%s”\n", match + strlen(match) + 1, newname);
    else
        printf("没改成（已经不在了？）%s\n", match + strlen(match) + 1);
    g_free(match);
    return ok ? 0 : 1;
}

static int cmd_unpair(const char *prefix)
{
    GError *e = NULL;
    GVariant *d;
    char *match;
    int status = 1;
    gboolean removed = FALSE;

    match = find_phone(prefix, &status);
    if (!match)
        return status;
    *strchr(match, '\t') = 0;
    d = admin("Unpair", g_variant_new("(s)", match), "(b)", &e);
    if (!d) {
        admin_fail("删除", e);
        g_free(match);
        return 1;
    }
    g_variant_get(d, "(b)", &removed);
    g_variant_unref(d);
    printf(removed ? "已删除手机“%s”\n" : "没删掉（已经不在了？）%s\n", match + strlen(match) + 1);
    g_free(match);
    return removed ? 0 : 1;
}

static void usage(void)
{
    fprintf(stderr,
            "用法（root）：\n"
            "  hisi-phone pair [用户名]   显示配对二维码，手机“电脑解锁”App 扫码（默认配给 sudo 前的用户）\n"
            "  hisi-phone list            列出已配对的手机\n"
            "  hisi-phone unpair <ID>     删除一台手机（ID 前缀即可）\n"
            "  hisi-phone rename <ID> <别名>     设手机别名（显示用，空串恢复手机原名；最长 64 字节，不用重新配对）\n"
            "说明见 docs/phone-unlock.md\n");
}

int phonectl_main(int argc, char **argv)
{
    GError *e = NULL;

    if (argc < 2) {
        usage();
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "要用 root 运行：sudo hisi-phone %s\n", argv[1]);
        return 2;
    }
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
    if (!bus) {
        fprintf(stderr, "连不上 system bus：%s\n", e->message);
        return 1;
    }
    if (!strcmp(argv[1], "pair")) {
        const char *user = argc > 2 ? argv[2] : g_getenv("SUDO_USER");
        struct passwd *pw;

        if (!user || !*user || !strcmp(user, "root")) {
            fprintf(stderr, "要配给哪个用户？sudo hisi-phone pair <用户名>\n");
            return 2;
        }
        pw = getpwnam(user);
        if (!pw || pw->pw_uid < 1000) {
            fprintf(stderr, "没有桌面用户 %s\n", user);
            return 2;
        }
        return cmd_pair(user);
    }
    if (!strcmp(argv[1], "list"))
        return cmd_list();
    if (!strcmp(argv[1], "unpair") && argc > 2)
        return cmd_unpair(argv[2]);
    if (!strcmp(argv[1], "rename") && argc > 3)
        return cmd_rename(argv[2], argv[3]);
    usage();
    return 2;
}
