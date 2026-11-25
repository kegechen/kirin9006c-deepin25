/*
 * NetworkManager 的 LD_PRELOAD 垫片（华为擎云 L540 / Hi1103 Wi-Fi）。
 *
 * 问题：NM >= 1.3x 用 NL80211_CMD_GET_STATION + NLM_F_DUMP 定时取信号强度/速率，
 *       Hi1103 厂商驱动的 dump_station 不会结束（同一个 AP 无限返回，iw station dump 3 秒 57 万条），
 *       NM 主线程在 nl80211_send_and_recv 里死循环，CPU 100%，D-Bus 无响应（控制中心随之卡死）。
 * 修法：拦截 sendmsg，把这条 dump 请求改成非 dump 的 GET_STATION + NL80211_ATTR_MAC(当前 BSSID) + NLM_F_ACK，
 *       BSSID 用 wext 的 SIOCGIWAP 取；取不到时不带 MAC，内核回 -EINVAL，NM 当作“暂无站点信息”处理。
 * 编译：gcc -O2 -shared -fPIC -o libnm-station-fix.so nm-station-fix.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

/* 不引 linux/wireless.h（和 net/if.h 冲突）；SIOCGIWAP 的结果在 ifreq 的 sockaddr 位置，与 iwreq.u.ap_addr 同偏移 */
#define SIOCGIWAP               0x8B15
#define NL80211_CMD_GET_STATION 17
#define NL80211_ATTR_IFINDEX    3
#define NL80211_ATTR_MAC        6

static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
static int logged;

static int get_bssid(int ifindex, unsigned char mac[6])
{
    static const unsigned char zero[6];
    char         name[IF_NAMESIZE];
    struct ifreq wrq;
    int          s, ok = -1;

    if (!if_indextoname(ifindex, name))
        return -1;
    s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    memset(&wrq, 0, sizeof wrq);
    strncpy(wrq.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIWAP, &wrq) == 0) {
        memcpy(mac, wrq.ifr_hwaddr.sa_data, 6);
        if (memcmp(mac, zero, 6) != 0)
            ok = 0;
    }
    close(s);
    return ok;
}

static int is_station_dump(int fd, const struct nlmsghdr *nlh, size_t len, int *ifindex)
{
    const struct genlmsghdr *g;
    const char              *p, *end;
    struct sockaddr_nl       sa;
    socklen_t                sl = sizeof sa;
    int                      proto;
    socklen_t                pl = sizeof proto;

    if (len < NLMSG_HDRLEN + GENL_HDRLEN + NLA_HDRLEN + 4 || nlh->nlmsg_len != len)
        return 0;
    if (nlh->nlmsg_type <= GENL_ID_CTRL || (nlh->nlmsg_flags & NLM_F_DUMP) != NLM_F_DUMP)
        return 0;
    g = NLMSG_DATA(nlh);
    if (g->cmd != NL80211_CMD_GET_STATION)
        return 0;
    if (getsockname(fd, (struct sockaddr *) &sa, &sl) != 0 || sa.nl_family != AF_NETLINK)
        return 0;
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &proto, &pl) != 0 || proto != NETLINK_GENERIC)
        return 0;

    *ifindex = -1;
    p        = (const char *) g + GENL_HDRLEN;
    end      = (const char *) nlh + len;
    while (p + NLA_HDRLEN <= end) {
        const struct nlattr *a = (const struct nlattr *) p;

        if (a->nla_len < NLA_HDRLEN || p + a->nla_len > end)
            break;
        if ((a->nla_type & NLA_TYPE_MASK) == NL80211_ATTR_IFINDEX && a->nla_len >= NLA_HDRLEN + 4)
            memcpy(ifindex, p + NLA_HDRLEN, 4);
        p += NLA_ALIGN(a->nla_len);
    }
    return *ifindex > 0;
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    int ifindex;

    if (!real_sendmsg)
        real_sendmsg = (ssize_t (*)(int, const struct msghdr *, int)) dlsym(RTLD_NEXT, "sendmsg");

    if (msg && msg->msg_iovlen == 1 && msg->msg_iov[0].iov_base
        && is_station_dump(fd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, &ifindex)) {
        size_t           len   = msg->msg_iov[0].iov_len;
        size_t           base  = NLMSG_ALIGN(len);
        size_t           extra = NLA_ALIGN(NLA_HDRLEN + 6);
        char            *buf   = calloc(1, base + extra);
        unsigned char    mac[6];
        struct nlmsghdr *n;
        struct iovec     iov;
        struct msghdr    m;
        ssize_t          r;
        int              e, have_mac;

        if (!buf)
            return real_sendmsg(fd, msg, flags);
        memcpy(buf, msg->msg_iov[0].iov_base, len);
        n              = (struct nlmsghdr *) buf;
        n->nlmsg_flags = (n->nlmsg_flags & ~NLM_F_DUMP) | NLM_F_ACK;
        n->nlmsg_len   = base;
        have_mac       = get_bssid(ifindex, mac) == 0;
        if (have_mac) {
            struct nlattr *a = (struct nlattr *) (buf + base);

            a->nla_type = NL80211_ATTR_MAC;
            a->nla_len  = NLA_HDRLEN + 6;
            memcpy((char *) a + NLA_HDRLEN, mac, 6);
            n->nlmsg_len = base + extra;
        }
        if (!logged) {
            logged = 1;
            syslog(LOG_INFO,
                   "nm-station-fix: rewrote nl80211 GET_STATION dump to single query (ifindex %d, bssid %s)",
                   ifindex,
                   have_mac ? "found" : "none");
        }
        iov       = (struct iovec){.iov_base = buf, .iov_len = n->nlmsg_len};
        m         = *msg;
        m.msg_iov = &iov;
        r         = real_sendmsg(fd, &m, flags);
        e         = errno;
        free(buf);
        errno = e;
        return r < 0 ? r : (ssize_t) len;
    }
    return real_sendmsg(fd, msg, flags);
}
