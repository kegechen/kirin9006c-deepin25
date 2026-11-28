/*
 * hisi-bio-unlock 内部共用的声明（hisi-bio-unlock.c / hisi-bio-phone.c / hisi-bio-phonectl.c）。
 * 手机解锁的协议见 docs/phone-unlock.md。
 */
#pragma once
#include <gio/gio.h>
#include <stdio.h>

#define LOG(pri, fmt, ...) fprintf(stderr, "<%d>" fmt "\n", pri, ##__VA_ARGS__)
#define INFO(fmt, ...)     LOG(6, fmt, ##__VA_ARGS__)
#define WARN(fmt, ...)     LOG(4, fmt, ##__VA_ARGS__)
#define DBG(fmt, ...)      do { if (verbose) LOG(7, fmt, ##__VA_ARGS__); } while (0)

#define UI_SVC      "org.hisi.BioUnlock1"
#define UI_PATH     "/org/hisi/BioUnlock1"
#define UI_IFACE    "org.hisi.BioUnlock1"
#define PHONE_IFACE "org.hisi.BioUnlock1.Phone"   /* 只给 hisi-phone-link（手机的 Wi-Fi / 蓝牙收发，用户 hisi-phone） */
#define ADMIN_IFACE "org.hisi.BioUnlock1.Admin"   /* 只给 root：配对、列出、删除手机 */
#define USER_IFACE  "org.hisi.BioUnlock1.User"    /* 桌面用户（控制中心插件）：只管自己的手机，配对要过 polkit 认证 */
#define POLKIT_PAIR_ACTION "org.hisi.bio-unlock.pair-phone"

#define PHONE_STATE_DIR  "/var/lib/hisi-bio"
#define PHONE_STORE      PHONE_STATE_DIR "/phones.ini"

/* 手机解锁协议的状态码（hisi-phone-link 原样转给手机，含义见 docs/phone-unlock.md） */
enum {
    PS_OK,
    PS_UNKNOWN_PHONE,   /* 没配对或已被删除 */
    PS_NO_SESSION,      /* 该用户没有本地图形会话 */
    PS_BAD_SIGNATURE,
    PS_BAD_CHALLENGE,   /* 过期或已用过 */
    PS_LOCKED_OUT,      /* 签名错太多次，暂停一会儿 */
    PS_PAIR_CLOSED,     /* 电脑上没有在配对 */
    PS_BAD_CODE,        /* 配对码校验不过 */
    PS_NOT_LOCKED,      /* 没锁屏（不给截图） */
    PS_BUSY,
    PS_ERROR,
    PS_BAD_REQUEST,
    PS_UNAVAILABLE,     /* hisi-bio-unlock 没运行（hisi-phone-link 自己报） */
    PS_TOO_MANY,        /* 该用户已绑定到上限（PHONES_PER_USER），新手机要先删一台 */
};

/* 每个桌面用户最多绑定的手机数（重新配对已有的手机不算新增） */
#define PHONES_PER_USER 5

extern int verbose;
extern GDBusConnection *bus;

/* hisi-bio-unlock.c 提供给手机解锁用 */
void bio_session_info(const char *user, gboolean *has_session, gboolean *locked, gint64 *locked_for_s);
int bio_unlock_user(const char *user, const char *how);  /* 解锁该用户的本地图形会话，返回 PS_* */
int bio_lock_user(const char *user, const char *how);    /* 锁定该用户的本地图形会话（已锁也回 PS_OK），返回 PS_* */
void bio_ui_refresh(void);                               /* 手机连接状态变了：刷新锁屏界面 */

/* hisi-bio-phone.c */
void phone_init(void);
/* 该用户现在连着电脑的手机里最近连上的那台的名字；一台都没有返回 NULL。*count 是连着的台数 */
const char *phone_online_name(const char *user, guint *count);

/* hisi-bio-phonectl.c：hisi-bio-unlock phone pair / list / unpair（root 运行的客户端） */
int phonectl_main(int argc, char **argv);
