package org.hisi.phoneunlock.proto;

import java.net.InetAddress;

/**
 * 判断一个目标地址是不是“局域网地址”（连它要走 Wi-Fi）。
 *
 * 当默认网络不是 Wi-Fi（例如连的 Wi-Fi 没有互联网，系统改走了移动数据）时，App 会把连局域网地址的 socket 绑到 Wi-Fi 上。
 * 但 Tailscale 的地址（IPv4 100.64.0.0/10、IPv6 fd7a:115c:a1e0::/48）、公网地址不在 Wi-Fi 那个网段里：
 * 硬绑到 Wi-Fi 会走不通，让系统按默认路由走（Tailscale 的 VPN、移动数据）才对。
 */
public final class LanAddress {
    private LanAddress() {}

    /** 私网（10/8、172.16/12、192.168/16）、链路本地（169.254/16、fe80::/10）、IPv6 唯一本地地址（fc00::/7，Tailscale 的除外）。 */
    public static boolean isLan(InetAddress a) {
        if (a.isLoopbackAddress() || a.isAnyLocalAddress() || a.isMulticastAddress()) return false;
        if (isTailscale(a)) return false;
        if (a.isSiteLocalAddress() || a.isLinkLocalAddress()) return true;
        byte[] b = a.getAddress();
        return b.length == 16 && (b[0] & 0xfe) == 0xfc;
    }

    /** Tailscale 分配的地址：IPv4 100.64.0.0/10（CGNAT 段），IPv6 fd7a:115c:a1e0::/48。 */
    public static boolean isTailscale(InetAddress a) {
        byte[] b = a.getAddress();
        if (b.length == 4) return (b[0] & 0xff) == 100 && (b[1] & 0xc0) == 0x40;
        return b.length == 16 && (b[0] & 0xff) == 0xfd && (b[1] & 0xff) == 0x7a && (b[2] & 0xff) == 0x11
                && (b[3] & 0xff) == 0x5c && (b[4] & 0xff) == 0xa1 && (b[5] & 0xff) == 0xe0;
    }
}
