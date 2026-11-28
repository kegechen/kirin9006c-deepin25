package org.hisi.phoneunlock.proto;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** 要连的那台电脑：pc_id、可直连的 TCP 地址、UDP 发现要单播的 IP、蓝牙地址。 */
public final class Target {
    public final String pcId;
    /** 直接试 TCP 的地址：二维码 IP（配 p 端口）+ 用户填的额外地址。 */
    public final List<HostPort> direct;
    /** UDP 发现除了广播之外还要单播的 IP（二维码里的 IP）。 */
    public final List<String> probeIps;
    /** 大写蓝牙地址，可为空串。 */
    public final String bt;

    public Target(String pcId, List<String> qrIps, int port, List<HostPort> extras, String bt) {
        this.pcId = pcId;
        List<HostPort> d = new ArrayList<>();
        for (String ip : qrIps) {
            HostPort hp = new HostPort(ip, port);
            if (!d.contains(hp)) d.add(hp);
        }
        if (extras != null) {
            for (HostPort hp : extras) {
                if (!d.contains(hp)) d.add(hp);
            }
        }
        this.direct = Collections.unmodifiableList(d);
        this.probeIps = Collections.unmodifiableList(new ArrayList<>(qrIps));
        this.bt = bt == null ? "" : bt;
    }
}
