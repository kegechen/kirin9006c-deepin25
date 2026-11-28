package org.hisi.phoneunlock.proto;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * 配对二维码：hisiphone://pair?v=1&id=&lt;pc_id&gt;&c=&lt;code&gt;&h=&lt;主机名&gt;&u=&lt;用户名&gt;&ip=&lt;ip1,ip2&gt;&p=47390&bt=&lt;蓝牙地址&gt;
 */
public final class PairUri {
    public static final String PREFIX = "hisiphone://pair";

    private static final Pattern BT = Pattern.compile("^([0-9A-F]{2}:){5}[0-9A-F]{2}$");

    public final String pcId;
    /** 16 字节配对码。用完请 Arrays.fill 清零。 */
    public final byte[] code;
    public final String host;
    public final String user;
    public final List<String> ips;
    public final int port;
    /** 大写的蓝牙地址，没有就是空串。 */
    public final String bt;

    private PairUri(String pcId, byte[] code, String host, String user, List<String> ips, int port, String bt) {
        this.pcId = pcId;
        this.code = code;
        this.host = host;
        this.user = user;
        this.ips = Collections.unmodifiableList(ips);
        this.port = port;
        this.bt = bt;
    }

    public static boolean looksLike(String s) {
        return s != null && s.trim().toLowerCase(Locale.ROOT).startsWith(PREFIX);
    }

    /** 解析失败抛 IllegalArgumentException，消息是中文。 */
    public static PairUri parse(String uri) {
        if (uri == null) throw new IllegalArgumentException("配对链接为空");
        String s = uri.trim();
        if (!looksLike(s)) throw new IllegalArgumentException("不是电脑解锁的配对链接（应以 hisiphone://pair? 开头）");
        int q = s.indexOf('?');
        if (q < 0 || q != PREFIX.length()) throw new IllegalArgumentException("配对链接缺少参数");
        String query = s.substring(q + 1);
        int hash = query.indexOf('#');
        if (hash >= 0) query = query.substring(0, hash);

        Map<String, String> kv = new HashMap<>();
        for (String part : query.split("&")) {
            if (part.isEmpty()) continue;
            int eq = part.indexOf('=');
            String k = pct(eq < 0 ? part : part.substring(0, eq));
            String v = eq < 0 ? "" : pct(part.substring(eq + 1));
            kv.put(k, v);
        }

        String v = kv.get("v");
        if (v == null) throw new IllegalArgumentException("配对链接缺少版本号 v");
        if (!v.equals(Integer.toString(Proto.VERSION))) {
            throw new IllegalArgumentException("配对链接版本不支持（v=" + v + "），请更新手机 App");
        }

        String id = kv.get("id");
        if (!Proto.isHexId(id)) throw new IllegalArgumentException("配对链接里的电脑 ID 不对");

        String c = kv.get("c");
        byte[] code;
        try {
            code = c == null ? null : Base64.getUrlDecoder().decode(c);
        } catch (IllegalArgumentException e) {
            code = null;
        }
        if (code == null || code.length != Proto.CODE_LEN) throw new IllegalArgumentException("配对链接里的配对码不对");

        String host = nz(kv.get("h"));
        String user = nz(kv.get("u"));

        List<String> ips = new ArrayList<>();
        for (String ip : nz(kv.get("ip")).split(",")) {
            String t = ip.trim();
            if (!t.isEmpty() && !ips.contains(t)) ips.add(t);
        }

        int port = Proto.DEFAULT_PORT;
        String p = kv.get("p");
        if (p != null && !p.isEmpty()) {
            try {
                port = Integer.parseInt(p);
            } catch (NumberFormatException e) {
                port = -1;
            }
            if (port < 1 || port > 65535) throw new IllegalArgumentException("配对链接里的端口不对");
        }

        String bt = nz(kv.get("bt")).trim().toUpperCase(Locale.ROOT);
        if (!bt.isEmpty() && !BT.matcher(bt).matches()) {
            Logger.i("二维码里的蓝牙地址格式不对，忽略：" + bt);
            bt = "";
        }
        return new PairUri(id, code, host, user, ips, port, bt);
    }

    private static String nz(String s) {
        return s == null ? "" : s;
    }

    /** 百分号解码（UTF-8）。不把 '+' 当空格。 */
    static String pct(String s) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (int i = 0; i < s.length(); ) {
            char ch = s.charAt(i);
            if (ch == '%' && hexVal(s, i + 1) >= 0 && hexVal(s, i + 2) >= 0) {
                out.write((hexVal(s, i + 1) << 4) | hexVal(s, i + 2));
                i += 3;
            } else {
                int cp = s.codePointAt(i);
                byte[] b = Proto.utf8(new String(Character.toChars(cp)));
                out.write(b, 0, b.length);
                i += Character.charCount(cp);
            }
        }
        return Proto.str(out.toByteArray());
    }

    private static int hexVal(String s, int i) {
        if (i >= s.length()) return -1;
        char c = s.charAt(i);
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
}
