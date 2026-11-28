import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import org.hisi.phoneunlock.proto.Client;
import org.hisi.phoneunlock.proto.HostPort;
import org.hisi.phoneunlock.proto.PairUri;
import org.hisi.phoneunlock.proto.PeerException;
import org.hisi.phoneunlock.proto.Proto;
import org.hisi.phoneunlock.proto.Target;
import org.hisi.phoneunlock.proto.TcpTransport;
import org.hisi.phoneunlock.proto.WifiConnector;
import org.hisi.phoneunlock.proto.WrongPcException;

/**
 * 纯 JVM 自测：
 *  1. 用固定输入打印 T、mac、mac2、S、ks、box、U 等十六进制，方便和电脑端（C + OpenSSL）对拍；
 *  2. 配对链接 / 地址解析；
 *  3. 和本进程里的 FakePc 走一遍 PAIR → HELLO → SHOT → UNLOCK（TCP 回环），以及 UDP 发现。
 */
public final class SelfTest {
    static int failures;

    static void check(boolean ok, String what) {
        System.out.println((ok ? "  [ok]   " : "  [FAIL] ") + what);
        if (!ok) failures++;
    }

    static void p(String k, byte[] v) {
        System.out.println(k + " (" + v.length + " 字节) = " + Proto.hex(v));
    }

    public static void main(String[] args) throws Exception {
        vectors();
        parsing();
        loopback();
        System.out.println();
        if (failures == 0) {
            System.out.println("全部通过");
        } else {
            System.out.println("失败 " + failures + " 项");
            System.exit(1);
        }
    }

    static void vectors() throws Exception {
        System.out.println("== 测试向量 ==");
        String pcId = "00112233445566778899aabbccddeeff";
        String phoneId = "ffeeddccbbaa99887766554433221100";
        byte[] code = Proto.fill(16, 0x01);
        byte[] name = Proto.utf8("TestPhone");
        byte[] pub = Proto.fill(32, 0x02);
        byte[] cn = Proto.fill(32, 0x03);
        byte[] pn = Proto.fill(16, 0x04);
        byte[] iv = Proto.fill(12, 0x05);

        System.out.println("pc_id    = " + pcId + "（按 32 个 ASCII 字符参与拼接）");
        System.out.println("phone_id = " + phoneId);
        System.out.println("code     = " + Proto.hex(code));
        System.out.println("name     = TestPhone");
        System.out.println("pub      = 32 x 02");
        System.out.println("cn       = 32 x 03");
        System.out.println("pn       = 16 x 04");
        System.out.println("派生标签 = " + Proto.S_PAIR + " / " + Proto.S_PAIR_OK + " / " + Proto.S_SECRET + " / "
                + Proto.S_SESSION + " / " + Proto.S_SHOT + " / " + Proto.S_UNLOCK + " / " + Proto.S_LOCK + " / " + Proto.S_AWAY);
        System.out.println();

        byte[] t = Proto.pairT(pcId, phoneId, name, pub);
        System.out.println("T 长度 = " + t.length);
        p("T", t);
        byte[] mac = Proto.pairMac(code, t);
        p("mac", mac);
        byte[] mac2 = Proto.pairMac2(code, t);
        p("mac2", mac2);
        byte[] s = Proto.secret(code, pcId, phoneId);
        p("S", s);
        byte[] ks = Proto.sessionKey(s, cn, pn);
        p("ks", ks);
        byte[] box = Proto.box(ks, Proto.AAD_INFO, iv, Proto.utf8("user=uos\n"));
        p("box(ks, iv=12x05, \"hisi-info-v1\", \"user=uos\\n\")", box);
        System.out.println("  其中 iv = " + Proto.hex(Arrays.copyOfRange(box, 0, 12))
                + "，密文 = " + Proto.hex(Arrays.copyOfRange(box, 12, box.length - 16))
                + "，tag = " + Proto.hex(Arrays.copyOfRange(box, box.length - 16, box.length)));
        byte[] plain = Proto.unbox(ks, Proto.AAD_INFO, box);
        System.out.println("box 解密 = " + Proto.parseInfo(Proto.str(plain)) + "（原文 " + Proto.hex(plain) + "）");
        byte[] u = Proto.unlockU(pcId, phoneId, cn, pn);
        System.out.println("U 长度 = " + u.length);
        p("U", u);
        p("SHOT 720 的 mac", Proto.shotMac(ks, 720));
        p("LOCK 的 mac", Proto.lockMac(ks));
        p("AWAY 30 的 mac", Proto.awayMac(ks, 30));
        p("AWAY 0 的 mac", Proto.awayMac(ks, 0));
        System.out.println();
        System.out.println("PAIR 行  = " + Proto.pairLine(phoneId, name, pub, mac));
        System.out.println("HELLO 行 = " + Proto.helloLine(phoneId, pn));
        System.out.println("CHAL 行  = CHAL " + Proto.b64(cn) + " " + Proto.b64(box));
        System.out.println("SHOT 行  = " + Proto.shotLine(720, Proto.shotMac(ks, 720)));
        System.out.println("LOCK 行  = " + Proto.lockLine(Proto.lockMac(ks)));
        System.out.println("AWAY 行  = " + Proto.awayLine(30, Proto.awayMac(ks, 30)));
        System.out.println("发现请求 = " + Proto.discoverLine(pcId).replace("\n", "\\n"));
        System.out.println();

        check(t.length == 18 + 1 + 32 + 1 + 32 + 1 + 9 + 1 + 32, "T 长度 = 127");
        check(u.length == 20 + 1 + 32 + 1 + 32 + 1 + 32 + 16, "U 长度 = 135");
        check(Arrays.equals(plain, Proto.utf8("user=uos\n")), "box 能解回原文");
        check(box.length == 12 + 9 + 16, "box 长度 = iv + 密文 + tag");
        check(Proto.S_SECRET.equals(new String(new char[] {'h', 'i', 's', 'i', '-', 'p', 'h', 'o', 'n', 'e', '-',
                's', 'e', 'c', 'r', 'e', 't', '-', 'v', '1'})), "S 的派生标签逐字符正确");
        boolean bad = false;
        try {
            Proto.unbox(ks, Proto.AAD_SHOT, box);
        } catch (java.security.GeneralSecurityException e) {
            bad = true;
        }
        check(bad, "aad 不同就解不开");
        bad = false;
        byte[] tampered = box.clone();
        tampered[14] ^= 1;
        try {
            Proto.unbox(ks, Proto.AAD_INFO, tampered);
        } catch (java.security.GeneralSecurityException e) {
            bad = true;
        }
        check(bad, "密文被改就解不开");
        check(Proto.phoneName("一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十").length <= 64, "手机名截到 64 字节以内");
        check(Arrays.equals(Proto.lockMac(ks), Proto.hmac(ks, Proto.utf8("hisi-phone-lock-v1"))), "LOCK 的 mac 没有结尾 0 字节");
        check(Arrays.equals(Proto.awayMac(ks, 30), Proto.hmac(ks, Proto.utf8("hisi-phone-away-v1\u000030"))),
                "AWAY 的 mac = 标签 ‖ 0 ‖ 十进制秒数");
        check(Proto.validGrace(0) && Proto.validGrace(5) && Proto.validGrace(600) && !Proto.validGrace(4)
                && !Proto.validGrace(601) && !Proto.validGrace(-1), "AWAY 秒数范围 0 / 5～600");

        KeyPairGenerator g = KeyPairGenerator.getInstance("EC");
        g.initialize(new ECGenParameterSpec("secp256r1"));
        KeyPair kp = g.generateKeyPair();
        Signature sg = Signature.getInstance("SHA256withECDSA");
        sg.initSign(kp.getPrivate());
        sg.update(u);
        byte[] sig = sg.sign();
        Signature vf = Signature.getInstance("SHA256withECDSA");
        vf.initVerify(kp.getPublic());
        vf.update(u);
        check(vf.verify(sig), "SHA256withECDSA 对 U 签名可验（DER " + sig.length + " 字节，公钥 SPKI " + kp.getPublic().getEncoded().length + " 字节）");
    }

    static void parsing() {
        System.out.println("== 解析 ==");
        PairUri pu = PairUri.parse("hisiphone://pair?v=1&id=0123456789abcdef0123456789abcdef&c=AAAAAAAAAAAAAAAAAAAAAA"
                + "&h=my%20pc&u=%E5%BC%A0%E4%B8%89&ip=192.0.2.1,198.51.100.7&p=47390&bt=aa:bb:cc:dd:ee:ff");
        check(pu.pcId.equals("0123456789abcdef0123456789abcdef"), "id");
        check(pu.code.length == 16 && Arrays.equals(pu.code, new byte[16]), "c 是 16 个 0");
        check(pu.host.equals("my pc"), "h 百分号解码");
        check(pu.user.equals("张三"), "u UTF-8 百分号解码");
        check(pu.ips.equals(Arrays.asList("192.0.2.1", "198.51.100.7")), "ip 列表");
        check(pu.port == 47390, "p");
        check(pu.bt.equals("AA:BB:CC:DD:EE:FF"), "bt 转大写");
        PairUri empty = PairUri.parse("hisiphone://pair?v=1&id=0123456789abcdef0123456789abcdef&c=AAAAAAAAAAAAAAAAAAAAAA&h=test&u=uos&ip=&p=47390&bt=");
        check(empty.ips.isEmpty() && empty.bt.isEmpty(), "ip、bt 为空");
        check(PairUri.parse("hisiphone://pair?v=1&id=0123456789abcdef0123456789abcdef&c=AAAAAAAAAAAAAAAAAAAAAA&h=a+b&u=u").host.equals("a+b"),
                "'+' 不当空格");
        check(throwsIae("hisiphone://pair?v=2&id=0123456789abcdef0123456789abcdef&c=AAAAAAAAAAAAAAAAAAAAAA"), "v=2 拒绝");
        check(throwsIae("hisiphone://pair?v=1&id=0123456789ABCDEF0123456789ABCDEF&c=AAAAAAAAAAAAAAAAAAAAAA"), "大写 id 拒绝");
        check(throwsIae("hisiphone://pair?v=1&id=0123456789abcdef0123456789abcdef&c=AAAA"), "配对码长度不对拒绝");
        check(throwsIae("https://example.com/"), "别的链接拒绝");
        List<HostPort> l = HostPort.parseList("127.0.0.1, 192.168.1.10:5000，[::1]:7 pc.local", 47390);
        check(l.size() == 4 && l.get(0).port == 47390 && l.get(1).port == 5000 && l.get(2).host.equals("::1")
                && l.get(2).port == 7 && l.get(3).host.equals("pc.local"), "额外地址解析：" + l);
    }

    static boolean throwsIae(String s) {
        try {
            PairUri.parse(s);
            return false;
        } catch (IllegalArgumentException e) {
            return true;
        }
    }

    static void loopback() throws Exception {
        System.out.println("== 回环（FakePc）==");
        String pcId = "0123456789abcdef0123456789abcdef";
        byte[] code = Proto.random(16);
        FakePc pc = new FakePc(pcId, code, "fake-pc", "uos", "测试用户");
        int port = pc.startTcp("127.0.0.1", 0);
        try {
            Target target = new Target(pcId, Collections.<String>emptyList(), port,
                    Collections.singletonList(new HostPort("127.0.0.1", port)), "");
            TcpTransport tr = new WifiConnector(target, null).connect();
            Client c = new Client(tr);

            KeyPairGenerator g = KeyPairGenerator.getInstance("EC");
            g.initialize(new ECGenParameterSpec("secp256r1"));
            KeyPair kp = g.generateKeyPair();
            String phoneId = Proto.newPhoneId();
            byte[] name = Proto.phoneName("JVM 测试机");
            byte[] pub = kp.getPublic().getEncoded();
            byte[] t = Proto.pairT(pcId, phoneId, name, pub);

            boolean e7 = false;
            try {
                c.pair(phoneId, name, pub, Proto.pairMac(Proto.random(16), t), 3000);
            } catch (PeerException e) {
                e7 = e.code == 7;
            }
            check(e7, "配对码不对 → ERR 7");

            byte[] mac2 = c.pair(phoneId, name, pub, Proto.pairMac(code, t), 3000);
            check(Proto.macEquals(mac2, Proto.pairMac2(code, t)), "PAIROK 的 mac2 核对通过");
            byte[] s = Proto.secret(code, pcId, phoneId);
            check(Arrays.equals(s, pc.phones.get(phoneId).s), "两边 S 一致");

            Client.Chal ch = c.hello(phoneId, s, 3000);
            check(ch.get("user").equals("uos") && ch.get("realname").equals("测试用户") && ch.get("session").equals("1"),
                    "HELLO 的 info：" + ch.info);
            byte[] jpg = c.shot(ch, 720, 5000);
            check(jpg.length > 100 && (jpg[0] & 0xff) == 0xff && (jpg[1] & 0xff) == 0xd8, "SHOT 拿到 JPEG（" + jpg.length + " 字节）");

            Signature sg = Signature.getInstance("SHA256withECDSA");
            sg.initSign(kp.getPrivate());
            sg.update(Proto.unlockU(pcId, phoneId, ch.cn, ch.pn));
            byte[] sig = sg.sign();
            c.unlock(sig, 3000);
            check(true, "UNLOCK → OK");
            boolean e4 = false;
            try {
                c.unlock(sig, 3000);
            } catch (PeerException e) {
                e4 = e.code == 4;
            }
            check(e4, "同一个 cn 再用 → ERR 4");

            Client.Chal ch2 = c.hello(phoneId, s, 3000);
            sg.initSign(kp.getPrivate());
            sg.update(Proto.unlockU(pcId, phoneId, ch.cn, ch.pn)); /* 故意签旧 cn */
            boolean e3 = false;
            try {
                c.unlock(sg.sign(), 3000);
            } catch (PeerException e) {
                e3 = e.code == 3;
            }
            check(e3 && ch2.cn.length == 32, "签旧挑战 → ERR 3");

            /* LOCK：不用签名，ks 的 HMAC；cn 用一次就作废 */
            pc.locked = false;
            Client.Chal ch3 = c.hello(phoneId, s, 3000);
            c.lock(ch3, 3000);
            check(pc.locked && pc.lockCount == 1 && ch3.used, "LOCK → OK，电脑锁上了");
            boolean l4 = false;
            try {
                c.lock(ch3, 3000);
            } catch (PeerException e) {
                l4 = e.code == 4;
            }
            check(l4 && pc.lockCount == 1, "同一个 cn 再 LOCK → ERR 4");
            Client.Chal ch4 = c.hello(phoneId, s, 3000);
            tr.sendLine(Proto.lockLine(Proto.lockMac(Proto.random(32))));
            String badLock = tr.readLine(3000);
            check(badLock.startsWith("ERR 3 ") && pc.lockCount == 1, "LOCK 的 mac 不对 → ERR 3（" + badLock.split(" ")[0] + " " + badLock.split(" ")[1] + "）");
            boolean l4b = false;
            try {
                c.lock(ch4, 3000);
            } catch (PeerException e) {
                l4b = e.code == 4;
            }
            check(l4b, "mac 不对那次也作废了 cn → 再 LOCK 是 ERR 4");

            /* AWAY：Wi-Fi（TCP）上电脑不收 */
            Client.Chal ch5 = c.hello(phoneId, s, 3000);
            boolean a11 = false;
            try {
                c.away(ch5, 30, 3000);
            } catch (PeerException e) {
                a11 = e.code == 11;
            }
            check(a11, "TCP 上发 AWAY → ERR 11");
            pc.awayOverTcp = true;
            c.away(ch5, 30, 3000);
            check(pc.away == 30, "AWAY 30 → OK（假电脑放开 TCP 限制，只验 mac）");
            c.away(ch5, 0, 3000);
            check(pc.away == 0 && !ch5.used, "AWAY 0 → OK，cn 不作废");
            tr.sendLine(Proto.awayLine(60, Proto.awayMac(ch5.ks, 30)));
            String badAway = tr.readLine(3000);
            check(badAway.startsWith("ERR 3 ") && pc.away == 0, "AWAY 的 mac 和秒数对不上 → ERR 3");
            boolean iae = false;
            try {
                c.away(ch5, 3, 3000);
            } catch (IllegalArgumentException e) {
                iae = true;
            }
            check(iae, "AWAY 3 秒（小于 5）在手机这边就拒绝");
            pc.awayOverTcp = false;

            boolean wrong = false;
            try {
                c.hello(phoneId, Proto.random(32), 3000);
            } catch (WrongPcException e) {
                wrong = true;
            }
            check(wrong, "S 不对 → 解不开 box（WrongPcException）");

            boolean e1 = false;
            try {
                c.hello(Proto.newPhoneId(), s, 3000);
            } catch (PeerException e) {
                e1 = e.code == 1;
            }
            check(e1, "没配对的 phone_id → ERR 1");
            tr.close();

            /* 另一台电脑（pc_id 不同）在这个地址上：PING 对不上，应当连接失败 */
            Target other = new Target("ffffffffffffffffffffffffffffffff", Collections.<String>emptyList(), port,
                    Collections.singletonList(new HostPort("127.0.0.1", port)), "");
            boolean rejected = false;
            try {
                new WifiConnector(other, null).connect().close();
            } catch (java.io.IOException e) {
                rejected = true;
                System.out.println("         （预期的报错：" + e.getMessage() + "）");
            }
            check(rejected, "PONG 的 pc_id 不对 → 不用这个连接");

            /* UDP 发现：在 127.0.0.1:47390 上应答，探测单播到二维码 IP 127.0.0.1 */
            if (pc.startUdp("127.0.0.1", Proto.DISCOVERY_PORT, port)) {
                Target probe = new Target(pcId, Collections.singletonList("127.0.0.1"), 1, null, "");
                /* probe.direct 里只有 127.0.0.1:1（必然连不上），能连上只可能是靠 UDP 回包里的端口 */
                long t0 = System.currentTimeMillis();
                TcpTransport tu = new WifiConnector(probe, null).connect();
                check(tu.describe().endsWith(":" + port), "UDP 发现后连上 " + tu.describe() + "（" + (System.currentTimeMillis() - t0) + " ms）");
                tu.close();
            } else {
                System.out.println("  [skip] UDP 47390 被占用，跳过发现测试");
            }
        } finally {
            pc.stop();
        }
    }
}
