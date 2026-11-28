package org.hisi.phoneunlock.proto;

import java.io.IOException;
import java.net.ProtocolException;
import java.security.GeneralSecurityException;
import java.util.Collections;
import java.util.Map;

/** 手机这一端的协议会话：在一个 Transport 上依次发 PING / PAIR / HELLO / SHOT / UNLOCK / LOCK / AWAY。 */
public final class Client {

    /** 一次 HELLO 的结果。cn 只能用一次。 */
    public static final class Chal {
        public final byte[] cn;
        public final byte[] pn;
        public final byte[] ks;
        public final Map<String, String> info;
        public final long at;
        /** UNLOCK / LOCK 发出去后置 true（不管成败 cn 都作废）。 */
        public volatile boolean used;

        Chal(byte[] cn, byte[] pn, byte[] ks, Map<String, String> info) {
            this.cn = cn;
            this.pn = pn;
            this.ks = ks;
            this.info = Collections.unmodifiableMap(info);
            this.at = System.currentTimeMillis();
        }

        public String get(String key) {
            String v = info.get(key);
            return v == null ? "" : v;
        }
    }

    private final Transport t;

    public Client(Transport t) {
        this.t = t;
    }

    public Transport transport() {
        return t;
    }

    /** PING → PONG &lt;pc_id&gt;。 */
    public String ping(long timeoutMs) throws IOException {
        String[] f = request(Proto.pingLine(), "PONG", 2, timeoutMs);
        return f[1];
    }

    /** PAIR → PAIROK &lt;mac2&gt;，返回 mac2 原始字节（调用者核对）。 */
    public byte[] pair(String phoneId, byte[] name, byte[] pub, byte[] mac, long timeoutMs) throws IOException {
        String[] f = request(Proto.pairLine(phoneId, name, pub, mac), "PAIROK", 2, timeoutMs);
        return Proto.unb64(f[1]);
    }

    /** HELLO → CHAL；box 解不开抛 WrongPcException。 */
    public Chal hello(String phoneId, byte[] s, long timeoutMs) throws IOException {
        byte[] pn = Proto.random(Proto.PN_LEN);
        String[] f = request(Proto.helloLine(phoneId, pn), "CHAL", 3, timeoutMs);
        byte[] cn = Proto.unb64(f[1]);
        if (cn.length != Proto.CN_LEN) throw new ProtocolException("电脑给的挑战长度不对（" + cn.length + " 字节）");
        byte[] box = Proto.unb64(f[2]);
        byte[] ks = Proto.sessionKey(s, cn, pn);
        byte[] info;
        try {
            info = Proto.unbox(ks, Proto.AAD_INFO, box);
        } catch (GeneralSecurityException e) {
            throw new WrongPcException();
        }
        return new Chal(cn, pn, ks, Proto.parseInfo(Proto.str(info)));
    }

    /** SHOT → SHOTOK，返回 JPEG。 */
    public byte[] shot(Chal c, int width, long timeoutMs) throws IOException {
        String[] f = request(Proto.shotLine(width, Proto.shotMac(c.ks, width)), "SHOTOK", 2, timeoutMs);
        try {
            return Proto.unbox(c.ks, Proto.AAD_SHOT, Proto.unb64(f[1]));
        } catch (GeneralSecurityException e) {
            throw new ProtocolException("截图解不开");
        }
    }

    /** UNLOCK → OK。 */
    public void unlock(byte[] sig, long timeoutMs) throws IOException {
        request(Proto.unlockLine(sig), "OK", 1, timeoutMs);
    }

    /** LOCK → OK：锁屏，不用指纹。不管成败 cn 都作废。 */
    public void lock(Chal c, long timeoutMs) throws IOException {
        c.used = true;
        request(Proto.lockLine(Proto.lockMac(c.ks)), "OK", 1, timeoutMs);
    }

    /** AWAY → OK：离开自动锁屏，grace 0 = 关。只能走蓝牙；cn 不作废。 */
    public void away(Chal c, int grace, long timeoutMs) throws IOException {
        if (!Proto.validGrace(grace)) throw new IllegalArgumentException("离开锁屏秒数不对：" + grace);
        request(Proto.awayLine(grace, Proto.awayMac(c.ks, grace)), "OK", 1, timeoutMs);
    }

    private String[] request(String line, String expect, int minFields, long timeoutMs) throws IOException {
        String cmd = line.indexOf(' ') > 0 ? line.substring(0, line.indexOf(' ')) : line;
        Logger.i("→ " + cmd + "（" + t.describe() + "）");
        t.sendLine(line);
        String resp = t.readLine(timeoutMs);
        String[] f = resp.split(" ", -1);
        Logger.i("← " + f[0]);
        if (f[0].equals("ERR")) {
            int code = -1;
            if (f.length > 1) {
                try {
                    code = Integer.parseInt(f[1]);
                } catch (NumberFormatException ignored) {
                }
            }
            String msg = "";
            if (f.length > 2) {
                try {
                    msg = Proto.str(Proto.unb64(f[2]));
                } catch (ProtocolException e) {
                    msg = f[2];
                }
            }
            Logger.i("电脑返回错误 " + code + " " + msg);
            throw new PeerException(code, msg);
        }
        if (!f[0].equals(expect) || f.length < minFields) {
            String shown = resp.length() > 40 ? resp.substring(0, 40) + "…" : resp;
            throw new ProtocolException("电脑回复格式不对（" + cmd + " → " + shown + "）");
        }
        return f;
    }
}
