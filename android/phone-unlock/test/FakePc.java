import java.awt.Color;
import java.awt.Font;
import java.awt.GradientPaint;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.image.BufferedImage;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.X509EncodedKeySpec;
import java.text.SimpleDateFormat;
import java.util.Base64;
import java.util.Date;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

import javax.imageio.ImageIO;

import org.hisi.phoneunlock.proto.Proto;

/**
 * 只用于测试的“假电脑”：按 docs/phone-unlock.md 实现 PING / PAIR / HELLO / SHOT / UNLOCK / LOCK / AWAY（TCP）
 * 和 UDP 发现应答。
 * 密码学直接复用 App 的 Proto 类，所以它只能验证 App 的流程，不能代替和 C 端对拍。
 *
 *   ./selftest.sh fakepc            # 监听 127.0.0.1:47390，打印配对链接；手机用 adb reverse tcp:47390 tcp:47390 连过来
 */
public final class FakePc {
    static final class Phone {
        final String phoneId;
        final PublicKey pub;
        final byte[] s;

        Phone(String phoneId, PublicKey pub, byte[] s) {
            this.phoneId = phoneId;
            this.pub = pub;
            this.s = s;
        }
    }

    final String pcId;
    final byte[] code;
    final String host;
    final String user;
    final String realname;
    final Map<String, Phone> phones = new ConcurrentHashMap<>();
    volatile boolean locked = true;
    volatile boolean session = true;
    /** 真电脑只在蓝牙会话上收 AWAY（TCP 回 ERR 11）；自测要走 TCP 验 AWAY 的 mac，就打开这个。 */
    volatile boolean awayOverTcp;
    /** 最近一次 AWAY 设的秒数（0 = 关）。 */
    volatile int away;
    volatile int lockCount;
    final long lockedSince = System.currentTimeMillis() - 125_000;
    ServerSocket ss;
    DatagramSocket udp;

    FakePc(String pcId, byte[] code, String host, String user, String realname) {
        this.pcId = pcId;
        this.code = code;
        this.host = host;
        this.user = user;
        this.realname = realname;
    }

    int startTcp(String bind, int port) throws IOException {
        ss = new ServerSocket();
        ss.setReuseAddress(true);
        ss.bind(new InetSocketAddress(InetAddress.getByName(bind), port));
        Thread t = new Thread(() -> {
            while (!ss.isClosed()) {
                try {
                    Socket c = ss.accept();
                    Thread h = new Thread(() -> serve(c), "fakepc-conn");
                    h.setDaemon(true);
                    h.start();
                } catch (IOException e) {
                    return;
                }
            }
        }, "fakepc-accept");
        t.setDaemon(true);
        t.start();
        return ss.getLocalPort();
    }

    /** UDP 发现应答：收到 "HISI-PHONE? <pc_id>" 回 "HISI-PHONE! <pc_id> <tcpPort>"。 */
    boolean startUdp(String bind, int udpPort, int tcpPort) {
        try {
            udp = new DatagramSocket(new InetSocketAddress(InetAddress.getByName(bind), udpPort));
        } catch (IOException e) {
            System.err.println("[fakepc] UDP " + bind + ":" + udpPort + " 绑定失败：" + e.getMessage());
            return false;
        }
        Thread t = new Thread(() -> {
            byte[] b = new byte[512];
            while (!udp.isClosed()) {
                try {
                    DatagramPacket p = new DatagramPacket(b, b.length);
                    udp.receive(p);
                    String s = new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8).trim();
                    if (s.equals(Proto.DISCOVER_Q + " " + pcId)) {
                        byte[] r = (Proto.DISCOVER_A + " " + pcId + " " + tcpPort + "\n").getBytes(StandardCharsets.UTF_8);
                        udp.send(new DatagramPacket(r, r.length, p.getSocketAddress()));
                    }
                } catch (IOException e) {
                    return;
                }
            }
        }, "fakepc-udp");
        t.setDaemon(true);
        t.start();
        return true;
    }

    void stop() {
        try {
            if (ss != null) ss.close();
        } catch (IOException ignored) {
        }
        if (udp != null) udp.close();
    }

    /* 一条 TCP 连接 = 一个会话 */
    private void serve(Socket c) {
        String peer = c.getRemoteSocketAddress().toString();
        log("连接 " + peer);
        Phone phone = null;
        byte[] cn = null, pn = null, ks = null;
        boolean cnUsed = true;
        long cnAt = 0;
        try {
            c.setSoTimeout(90_000);
            BufferedReader in = new BufferedReader(new InputStreamReader(c.getInputStream(), StandardCharsets.UTF_8));
            OutputStream out = c.getOutputStream();
            String line;
            while ((line = in.readLine()) != null) {
                String[] f = line.split(" ", -1);
                String resp;
                try {
                    switch (f[0]) {
                        case "PING":
                            resp = "PONG " + pcId;
                            break;
                        case "PAIR": {
                            if (f.length != 6 || !f[1].equals("1") || !Proto.isHexId(f[2])) {
                                resp = err(11, "请求格式不对");
                                break;
                            }
                            byte[] name = Base64.getDecoder().decode(f[3]);
                            byte[] pub = Base64.getDecoder().decode(f[4]);
                            byte[] mac = Base64.getDecoder().decode(f[5]);
                            byte[] t = Proto.pairT(pcId, f[2], name, pub);
                            if (!Proto.macEquals(mac, Proto.pairMac(code, t))) {
                                resp = err(7, "配对码不对");
                                break;
                            }
                            PublicKey pk = KeyFactory.getInstance("EC").generatePublic(new X509EncodedKeySpec(pub));
                            phones.put(f[2], new Phone(f[2], pk, Proto.secret(code, pcId, f[2])));
                            log("配对成功：" + new String(name, StandardCharsets.UTF_8) + " phone_id=" + f[2]);
                            resp = "PAIROK " + Proto.b64(Proto.pairMac2(code, t));
                            break;
                        }
                        case "HELLO": {
                            if (f.length != 4 || !f[1].equals("1")) {
                                resp = err(11, "请求格式不对");
                                break;
                            }
                            phone = phones.get(f[2]);
                            if (phone == null) {
                                resp = err(1, "这部手机没有配对");
                                break;
                            }
                            pn = Base64.getDecoder().decode(f[3]);
                            if (pn.length != Proto.PN_LEN) {
                                resp = err(11, "pn 长度不对");
                                break;
                            }
                            cn = Proto.random(Proto.CN_LEN);
                            cnUsed = false;
                            cnAt = System.currentTimeMillis();
                            ks = Proto.sessionKey(phone.s, cn, pn);
                            StringBuilder info = new StringBuilder();
                            info.append("user=").append(user).append('\n');
                            info.append("realname=").append(realname).append('\n');
                            info.append("host=").append(host).append('\n');
                            info.append("session=").append(session ? 1 : 0).append('\n');
                            info.append("locked=").append(locked ? 1 : 0).append('\n');
                            if (locked) info.append("locked_for=").append((System.currentTimeMillis() - lockedSince) / 1000).append('\n');
                            info.append("shot=").append(locked && session ? 1 : 0).append('\n');
                            resp = "CHAL " + Proto.b64(cn) + " " + Proto.b64(Proto.box(ks, Proto.AAD_INFO, Proto.utf8(info.toString())));
                            break;
                        }
                        case "SHOT": {
                            if (ks == null || f.length != 3) {
                                resp = err(11, "要先 HELLO");
                                break;
                            }
                            int w = Integer.parseInt(f[1]);
                            if (!Proto.macEquals(Base64.getDecoder().decode(f[2]), Proto.shotMac(ks, w))) {
                                resp = err(11, "截图请求校验不对");
                                break;
                            }
                            if (!locked) {
                                resp = err(8, "电脑没有锁屏");
                                break;
                            }
                            w = Math.max(288, Math.min(960, w));
                            resp = "SHOTOK " + Proto.b64(Proto.box(ks, Proto.AAD_SHOT, jpeg(w)));
                            break;
                        }
                        case "UNLOCK": {
                            if (phone == null || cn == null || cnUsed || System.currentTimeMillis() - cnAt > 120_000) {
                                resp = err(4, "挑战过期或已用过");
                                break;
                            }
                            cnUsed = true;
                            if (f.length != 2) {
                                resp = err(11, "请求格式不对");
                                break;
                            }
                            Signature v = Signature.getInstance("SHA256withECDSA");
                            v.initVerify(phone.pub);
                            v.update(Proto.unlockU(pcId, phone.phoneId, cn, pn));
                            boolean ok;
                            try {
                                ok = v.verify(Base64.getDecoder().decode(f[1]));
                            } catch (java.security.SignatureException e) {
                                ok = false;
                            }
                            if (ok) {
                                log("解锁成功（模拟）");
                                locked = false;
                                resp = "OK";
                            } else {
                                resp = err(3, "签名不对");
                            }
                            break;
                        }
                        case "LOCK": {
                            /* 和 UNLOCK 一样：cn 不管成败都作废 */
                            if (phone == null || cn == null || cnUsed || System.currentTimeMillis() - cnAt > 120_000) {
                                resp = err(4, "挑战过期或已用过");
                                break;
                            }
                            cnUsed = true;
                            if (f.length != 2) {
                                resp = err(11, "请求格式不对");
                                break;
                            }
                            if (!Proto.macEquals(Base64.getDecoder().decode(f[1]), Proto.lockMac(ks))) {
                                resp = err(3, "锁屏请求校验不对");
                                break;
                            }
                            if (!session) {
                                resp = err(2, "没有图形会话");
                                break;
                            }
                            locked = true;
                            lockCount++;
                            log("锁屏（模拟）");
                            resp = "OK";
                            break;
                        }
                        case "AWAY": {
                            if (!awayOverTcp) {
                                resp = err(11, "离开自动锁屏只能走蓝牙");
                                break;
                            }
                            if (phone == null || cn == null || cnUsed || System.currentTimeMillis() - cnAt > 120_000) {
                                resp = err(4, "要先 HELLO");
                                break;
                            }
                            if (f.length != 3 || !f[1].matches("0|[1-9][0-9]{0,3}")) {
                                resp = err(11, "请求格式不对");
                                break;
                            }
                            int g = Integer.parseInt(f[1]);
                            if (g != 0 && !Proto.validGrace(g)) {
                                resp = err(11, "秒数不对");
                                break;
                            }
                            if (!Proto.macEquals(Base64.getDecoder().decode(f[2]), Proto.awayMac(ks, g))) {
                                resp = err(3, "离开锁屏设置校验不对");
                                break;
                            }
                            away = g;
                            log("离开自动锁屏 = " + g + " 秒（模拟）");
                            resp = "OK";
                            break;
                        }
                        default:
                            resp = err(11, "不认识的命令");
                    }
                } catch (Exception e) {
                    resp = err(11, "请求格式不对：" + e.getClass().getSimpleName());
                }
                log(f[0] + " → " + (resp.length() > 60 ? resp.substring(0, 60) + "…" : resp));
                out.write((resp + "\n").getBytes(StandardCharsets.UTF_8));
                out.flush();
            }
        } catch (IOException e) {
            log("连接 " + peer + " 结束：" + e.getMessage());
        } finally {
            try {
                c.close();
            } catch (IOException ignored) {
            }
        }
    }

    static String err(int code, String msg) {
        return "ERR " + code + " " + Proto.b64(Proto.utf8(msg));
    }

    static byte[] jpeg(int w) throws IOException {
        int h = w * 10 / 16;
        BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
        Graphics2D g = img.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
        g.setPaint(new GradientPaint(0, 0, new Color(0x1565C0), w, h, new Color(0x4A148C)));
        g.fillRect(0, 0, w, h);
        g.setColor(Color.WHITE);
        g.setFont(new Font(Font.SANS_SERIF, Font.BOLD, Math.max(12, w / 8)));
        g.drawString(new SimpleDateFormat("HH:mm").format(new Date()), w / 10, h / 2);
        g.setFont(new Font(Font.SANS_SERIF, Font.PLAIN, Math.max(10, w / 28)));
        g.drawString("FakePc lock screen  " + w + "x" + h, w / 10, h / 2 + w / 16);
        g.dispose();
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        ImageIO.write(img, "jpg", bo);
        return bo.toByteArray();
    }

    static void log(String s) {
        System.err.println("[fakepc] " + s);
    }

    static String pairLink(String pcId, byte[] code, String host, String user, String ips, int port, String bt) {
        return "hisiphone://pair?v=1&id=" + pcId
                + "&c=" + Base64.getUrlEncoder().withoutPadding().encodeToString(code)
                + "&h=" + pctEncode(host) + "&u=" + pctEncode(user)
                + "&ip=" + ips + "&p=" + port + "&bt=" + bt;
    }

    static String pctEncode(String s) {
        StringBuilder sb = new StringBuilder();
        for (byte b : s.getBytes(StandardCharsets.UTF_8)) {
            int c = b & 0xff;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
                sb.append((char) c);
            } else {
                sb.append('%').append(String.format("%02X", c));
            }
        }
        return sb.toString();
    }

    public static void main(String[] args) throws Exception {
        String bind = "127.0.0.1";
        int port = Proto.DEFAULT_PORT;
        String pcId = "0123456789abcdef0123456789abcdef";
        byte[] code = Proto.fill(16, 0x11);
        String host = "fake-pc";
        String user = "uos";
        String realname = "测试用户";
        String ips = "127.0.0.1";
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--bind": bind = args[++i]; break;
                case "--port": port = Integer.parseInt(args[++i]); break;
                case "--id": pcId = args[++i]; break;
                case "--code": code = Base64.getUrlDecoder().decode(args[++i]); break;
                case "--host": host = args[++i]; break;
                case "--user": user = args[++i]; break;
                case "--ip": ips = args[++i]; break;
                case "--unlocked": break;
                default: throw new IllegalArgumentException("不认识的参数 " + args[i]);
            }
        }
        FakePc pc = new FakePc(pcId, code, host, user, realname);
        for (String a : args) if (a.equals("--unlocked")) pc.locked = false;
        int p = pc.startTcp(bind, port);
        log("TCP 监听 " + bind + ":" + p + "，pc_id=" + pcId);
        String link = pairLink(pcId, code, host, user, ips, p, "");
        System.out.println(link);
        log("手机调试：adb reverse tcp:" + p + " tcp:" + p + "，然后");
        log("  adb shell \"am start -a android.intent.action.VIEW -d '" + link + "'\"");
        Thread.sleep(Long.MAX_VALUE);
    }
}
