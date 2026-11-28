import java.io.FileOutputStream;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;

import org.hisi.phoneunlock.proto.Client;
import org.hisi.phoneunlock.proto.HostPort;
import org.hisi.phoneunlock.proto.PairUri;
import org.hisi.phoneunlock.proto.PeerException;
import org.hisi.phoneunlock.proto.Proto;
import org.hisi.phoneunlock.proto.TcpTransport;

/**
 * 用 App 的协议层（软件 EC 密钥代替 Keystore）对真实电脑端走一遍：PING → PAIR → HELLO → SHOT → 错签名 → HELLO → UNLOCK
 * → HELLO → AWAY（Wi-Fi 上应被拒）→ 错 mac 的 LOCK →（--lock 时）HELLO → LOCK。
 *   ./selftest.sh realpc '<hisiphone://pair?...>' [host[:port]] [shot.jpg] [--no-unlock] [--lock]
 * 电脑上先 sudo hisi-phone pair 拿链接；Wi-Fi 不通时可经 ssh -L 47390 隧道用 127.0.0.1。
 */
public final class RealPc {
    public static void main(String[] a) throws Exception {
        PairUri u = PairUri.parse(a[0]);
        HostPort hp = a.length > 1 ? HostPort.parse(a[1], u.port) : new HostPort(u.ips.get(0), u.port);
        String shotFile = a.length > 2 ? a[2] : null;
        boolean doUnlock = true, doLock = false;
        for (int i = 3; i < a.length; i++) {
            if (a[i].equals("--no-unlock")) doUnlock = false;
            else if (a[i].equals("--lock")) doLock = true;
            else throw new IllegalArgumentException("不认识的参数 " + a[i]);
        }

        KeyPairGenerator g = KeyPairGenerator.getInstance("EC");
        g.initialize(new ECGenParameterSpec("secp256r1"));
        KeyPair kp = g.generateKeyPair();
        byte[] pub = kp.getPublic().getEncoded();
        String phoneId = Proto.newPhoneId();
        byte[] name = Proto.phoneName("JVM RealPc 测试");

        TcpTransport t = TcpTransport.open(hp, 3000, null);
        Client c = new Client(t);
        String pong = c.ping(5000);
        check(pong.equals(u.pcId), "PONG 的 pc_id 对得上");
        byte[] tt = Proto.pairT(u.pcId, phoneId, name, pub);
        byte[] mac2 = c.pair(phoneId, name, pub, Proto.pairMac(u.code, tt), 10000);
        check(Proto.macEquals(mac2, Proto.pairMac2(u.code, tt)), "PAIROK 的 mac2 校验通过");
        byte[] s = Proto.secret(u.code, u.pcId, phoneId);

        Client.Chal ch = c.hello(phoneId, s, 10000);
        System.out.println("电脑信息：" + ch.info);
        check(!ch.get("user").isEmpty(), "box 解开，含用户名");
        if (shotFile != null && ch.get("shot").equals("1")) {
            byte[] jpg = c.shot(ch, 720, 20000);
            try (FileOutputStream o = new FileOutputStream(shotFile)) {
                o.write(jpg);
            }
            check(jpg.length > 2 && (jpg[0] & 0xff) == 0xff && (jpg[1] & 0xff) == 0xd8, "截图是 JPEG（" + jpg.length + " 字节）→ " + shotFile);
        }
        // 错签名：签别的内容
        Signature sg = Signature.getInstance("SHA256withECDSA");
        sg.initSign(kp.getPrivate());
        sg.update(Proto.utf8("not the challenge"));
        try {
            c.unlock(sg.sign(), 10000);
            check(false, "错签名应被拒");
        } catch (PeerException e) {
            check(e.code == 3, "错签名被拒（ERR " + e.code + "）");
        }
        // 用过的 cn 不能再用
        try {
            sg.initSign(kp.getPrivate());
            sg.update(Proto.unlockU(u.pcId, phoneId, ch.cn, ch.pn));
            c.unlock(sg.sign(), 10000);
            check(false, "用过的挑战应被拒");
        } catch (PeerException e) {
            check(e.code == 4, "用过的挑战被拒（ERR " + e.code + "）");
        }
        if (doUnlock) {
            Client.Chal ch2 = c.hello(phoneId, s, 10000);
            sg.initSign(kp.getPrivate());
            sg.update(Proto.unlockU(u.pcId, phoneId, ch2.cn, ch2.pn));
            c.unlock(sg.sign(), 10000);
            check(true, "正确签名 → OK（已解锁）");
        }
        // AWAY 只收蓝牙：Wi-Fi 上应回 ERR 11
        Client.Chal ch3 = c.hello(phoneId, s, 10000);
        try {
            c.away(ch3, 30, 10000);
            check(false, "Wi-Fi 上的 AWAY 应被拒");
        } catch (PeerException e) {
            check(e.code == 11, "Wi-Fi 上的 AWAY 被拒（ERR " + e.code + "）");
        }
        // 错 mac 的 LOCK：拒绝，cn 作废
        t.sendLine(Proto.lockLine(Proto.lockMac(Proto.random(32))));
        String bad = t.readLine(10000);
        check(bad.startsWith("ERR 3 "), "错 mac 的 LOCK 被拒（" + bad.split(" ")[0] + " " + bad.split(" ")[1] + "）");
        try {
            c.lock(ch3, 10000);
            check(false, "错 mac 那次之后 cn 应已作废");
        } catch (PeerException e) {
            check(e.code == 4, "错 mac 那次之后 cn 作废（ERR " + e.code + "）");
        }
        if (doLock) {
            Client.Chal ch4 = c.hello(phoneId, s, 10000);
            c.lock(ch4, 10000);
            check(true, "LOCK → OK（已锁屏）");
        }
        t.close();
        System.out.println("全部通过；测试手机 ID " + phoneId + "（用完 sudo hisi-phone unpair " + phoneId.substring(0, 8) + "）");
    }

    private static void check(boolean ok, String what) {
        System.out.println((ok ? "  [通过] " : "  [失败] ") + what);
        if (!ok) System.exit(1);
    }
}
