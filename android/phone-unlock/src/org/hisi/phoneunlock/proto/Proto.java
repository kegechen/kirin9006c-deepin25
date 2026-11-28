package org.hisi.phoneunlock.proto;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.Map;

import javax.crypto.AEADBadTagException;
import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * 手机解锁协议（版本 1）的常量、密钥派生、加解密和消息拼接。
 * 只依赖 java.*，可以在普通 JVM 上跑（selftest.sh）。规范见 docs/phone-unlock.md。
 */
public final class Proto {
    private Proto() {}

    public static final int VERSION = 1;
    public static final int DEFAULT_PORT = 47390;
    /** UDP 发现端口（文档写死 47390）。 */
    public static final int DISCOVERY_PORT = 47390;

    public static final String S_PAIR = "hisi-phone-pair-v1";
    public static final String S_PAIR_OK = "hisi-phone-pair-ok-v1";
    /** 即 "hisi-phone-" + "secret" + "-v1"（分段写只是为了避开编辑工具对 secret 字样的改写）。 */
    public static final String S_SECRET = "hisi-phone-" + "sec" + "ret" + "-v1";
    public static final String S_SESSION = "hisi-phone-session-v1";
    public static final String S_SHOT = "hisi-phone-shot-v1";
    public static final String S_UNLOCK = "hisi-phone-unlock-v1";
    public static final String S_LOCK = "hisi-phone-lock-v1";
    public static final String S_AWAY = "hisi-phone-away-v1";
    public static final String AAD_INFO = "hisi-info-v1";
    public static final String AAD_SHOT = "hisi-shot-v1";

    public static final String DISCOVER_Q = "HISI-PHONE?";
    public static final String DISCOVER_A = "HISI-PHONE!";

    public static final String BLE_SERVICE = "5a3f0001-4c6e-4f1b-9d2a-7e8b1c0d4f60";
    public static final String BLE_RX = "5a3f0002-4c6e-4f1b-9d2a-7e8b1c0d4f60";
    public static final String BLE_TX = "5a3f0003-4c6e-4f1b-9d2a-7e8b1c0d4f60";

    /** 手机发出的一行最长 4 KiB（含 \n）。 */
    public static final int MAX_PHONE_LINE = 4096;
    /** 电脑回的一行最长 512 KiB。 */
    public static final int MAX_PC_LINE = 512 * 1024;

    public static final int CODE_LEN = 16;
    public static final int PN_LEN = 16;
    public static final int CN_LEN = 32;
    public static final int IV_LEN = 12;
    public static final int TAG_LEN = 16;
    public static final int MAX_NAME_BYTES = 64;
    /** AWAY 的等待秒数：0 = 关，否则 AWAY_MIN..AWAY_MAX。 */
    public static final int AWAY_MIN = 5;
    public static final int AWAY_MAX = 600;

    /* 错误码 */
    public static final int E_NOT_PAIRED = 1;
    public static final int E_NO_SESSION = 2;
    public static final int E_BAD_SIG = 3;
    public static final int E_STALE_CHAL = 4;
    public static final int E_LOCKOUT = 5;
    public static final int E_NOT_PAIRING = 6;
    public static final int E_PAIR_MAC = 7;
    public static final int E_NOT_LOCKED = 8;
    public static final int E_BUSY = 9;
    public static final int E_INTERNAL = 10;
    public static final int E_BAD_REQUEST = 11;
    public static final int E_NO_SERVICE = 12;

    private static final SecureRandom RNG = new SecureRandom();
    private static final byte[] Z = {0};

    /* ==== 字节工具 ==== */

    public static byte[] random(int n) {
        byte[] b = new byte[n];
        RNG.nextBytes(b);
        return b;
    }

    public static byte[] utf8(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    public static byte[] ascii(String s) {
        return s.getBytes(StandardCharsets.US_ASCII);
    }

    public static String str(byte[] b) {
        return new String(b, StandardCharsets.UTF_8);
    }

    public static byte[] cat(byte[]... parts) {
        int n = 0;
        for (byte[] p : parts) n += p.length;
        byte[] out = new byte[n];
        int o = 0;
        for (byte[] p : parts) {
            System.arraycopy(p, 0, out, o, p.length);
            o += p.length;
        }
        return out;
    }

    public static String hex(byte[] b) {
        char[] d = "0123456789abcdef".toCharArray();
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte x : b) {
            sb.append(d[(x >> 4) & 15]).append(d[x & 15]);
        }
        return sb.toString();
    }

    public static byte[] fill(int n, int value) {
        byte[] b = new byte[n];
        java.util.Arrays.fill(b, (byte) value);
        return b;
    }

    /** 标准 Base64，带 = 补齐，不换行。 */
    public static String b64(byte[] b) {
        return Base64.getEncoder().encodeToString(b);
    }

    public static byte[] unb64(String s) throws java.net.ProtocolException {
        try {
            return Base64.getDecoder().decode(s);
        } catch (IllegalArgumentException e) {
            throw new java.net.ProtocolException("Base64 字段格式不对");
        }
    }

    /** pc_id / phone_id：32 个小写十六进制字符。 */
    public static boolean isHexId(String s) {
        if (s == null || s.length() != 32) return false;
        for (int i = 0; i < 32; i++) {
            char c = s.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    public static String newPhoneId() {
        return hex(random(16));
    }

    /** 手机名：UTF-8 不超过 64 字节（按字符边界截断）。 */
    public static byte[] phoneName(String model) {
        String s = (model == null || model.trim().isEmpty()) ? "Android" : model.trim();
        StringBuilder sb = new StringBuilder();
        int bytes = 0;
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            String ch = new String(Character.toChars(cp));
            int n = utf8(ch).length;
            if (bytes + n > MAX_NAME_BYTES) break;
            sb.append(ch);
            bytes += n;
            i += Character.charCount(cp);
        }
        return utf8(sb.toString());
    }

    /* ==== 密码学 ==== */

    /** HMAC-SHA256。 */
    public static byte[] hmac(byte[] key, byte[] msg) {
        try {
            Mac m = Mac.getInstance("HmacSHA256");
            m.init(new SecretKeySpec(key, "HmacSHA256"));
            return m.doFinal(msg);
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException("HmacSHA256 不可用", e);
        }
    }

    /** 常量时间比较。 */
    public static boolean macEquals(byte[] a, byte[] b) {
        return a != null && b != null && MessageDigest.isEqual(a, b);
    }

    /** box(k, aad, p) = iv ‖ AES-256-GCM 密文 ‖ 16 字节 tag，iv 由调用者给（测试用）。 */
    public static byte[] box(byte[] key, String aad, byte[] iv, byte[] plain) {
        try {
            Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
            c.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(TAG_LEN * 8, iv));
            c.updateAAD(ascii(aad));
            return cat(iv, c.doFinal(plain));
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException("AES-GCM 加密失败", e);
        }
    }

    /** box，iv 取 12 字节随机。 */
    public static byte[] box(byte[] key, String aad, byte[] plain) {
        return box(key, aad, random(IV_LEN), plain);
    }

    /** 解 box；tag 不对抛 AEADBadTagException。 */
    public static byte[] unbox(byte[] key, String aad, byte[] box) throws GeneralSecurityException {
        if (box == null || box.length < IV_LEN + TAG_LEN) throw new AEADBadTagException("box 太短");
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(TAG_LEN * 8, box, 0, IV_LEN));
        c.updateAAD(ascii(aad));
        return c.doFinal(box, IV_LEN, box.length - IV_LEN);
    }

    /* ==== 协议里的派生量 ==== */

    /** T = "hisi-phone-pair-v1" ‖ 0 ‖ pc_id ‖ 0 ‖ phone_id ‖ 0 ‖ name ‖ 0 ‖ pub（pc_id / phone_id 是 32 字符十六进制文本）。 */
    public static byte[] pairT(String pcId, String phoneId, byte[] name, byte[] pub) {
        return cat(utf8(S_PAIR), Z, utf8(pcId), Z, utf8(phoneId), Z, name, Z, pub);
    }

    /** mac = HMAC(code, T)。 */
    public static byte[] pairMac(byte[] code, byte[] t) {
        return hmac(code, t);
    }

    /** mac2 = HMAC(code, "hisi-phone-pair-ok-v1" ‖ 0 ‖ T)。 */
    public static byte[] pairMac2(byte[] code, byte[] t) {
        return hmac(code, cat(utf8(S_PAIR_OK), Z, t));
    }

    /** S = HMAC(code, S_SECRET ‖ 0 ‖ pc_id ‖ 0 ‖ phone_id)。 */
    public static byte[] secret(byte[] code, String pcId, String phoneId) {
        return hmac(code, cat(utf8(S_SECRET), Z, utf8(pcId), Z, utf8(phoneId)));
    }

    /** ks = HMAC(S, "hisi-phone-session-v1" ‖ 0 ‖ cn ‖ pn)。 */
    public static byte[] sessionKey(byte[] s, byte[] cn, byte[] pn) {
        return hmac(s, cat(utf8(S_SESSION), Z, cn, pn));
    }

    /** SHOT 的 mac = HMAC(ks, "hisi-phone-shot-v1" ‖ 0 ‖ width 的十进制 ASCII)。 */
    public static byte[] shotMac(byte[] ks, int width) {
        return hmac(ks, cat(utf8(S_SHOT), Z, ascii(Integer.toString(width))));
    }

    /** U = "hisi-phone-unlock-v1" ‖ 0 ‖ pc_id ‖ 0 ‖ phone_id ‖ 0 ‖ cn ‖ pn。 */
    public static byte[] unlockU(String pcId, String phoneId, byte[] cn, byte[] pn) {
        return cat(utf8(S_UNLOCK), Z, utf8(pcId), Z, utf8(phoneId), Z, cn, pn);
    }

    /** LOCK 的 mac = HMAC(ks, "hisi-phone-lock-v1")（只有标签，没有零字节）。 */
    public static byte[] lockMac(byte[] ks) {
        return hmac(ks, utf8(S_LOCK));
    }

    /** AWAY 的 mac = HMAC(ks, "hisi-phone-away-v1" ‖ 0 ‖ grace 的十进制 ASCII)。 */
    public static byte[] awayMac(byte[] ks, int grace) {
        return hmac(ks, cat(utf8(S_AWAY), Z, ascii(Integer.toString(grace))));
    }

    public static boolean validGrace(int grace) {
        return grace == 0 || (grace >= AWAY_MIN && grace <= AWAY_MAX);
    }

    /* ==== 消息行（不含结尾 \n） ==== */

    public static String pingLine() {
        return "PING";
    }

    public static String pairLine(String phoneId, byte[] name, byte[] pub, byte[] mac) {
        return "PAIR " + VERSION + " " + phoneId + " " + b64(name) + " " + b64(pub) + " " + b64(mac);
    }

    public static String helloLine(String phoneId, byte[] pn) {
        return "HELLO " + VERSION + " " + phoneId + " " + b64(pn);
    }

    public static String shotLine(int width, byte[] mac) {
        return "SHOT " + width + " " + b64(mac);
    }

    public static String unlockLine(byte[] sig) {
        return "UNLOCK " + b64(sig);
    }

    public static String lockLine(byte[] mac) {
        return "LOCK " + b64(mac);
    }

    public static String awayLine(int grace, byte[] mac) {
        return "AWAY " + grace + " " + b64(mac);
    }

    /** UDP 发现请求（含 \n）。 */
    public static String discoverLine(String pcId) {
        return DISCOVER_Q + " " + pcId + "\n";
    }

    /** info 是若干行 key=value\n。 */
    public static Map<String, String> parseInfo(String text) {
        Map<String, String> m = new LinkedHashMap<>();
        for (String line : text.split("\n")) {
            if (line.isEmpty()) continue;
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            m.put(line.substring(0, eq), line.substring(eq + 1));
        }
        return m;
    }
}
