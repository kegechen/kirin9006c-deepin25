package org.hisi.phoneunlock;

import org.hisi.phoneunlock.proto.HostPort;
import org.hisi.phoneunlock.proto.Proto;
import org.hisi.phoneunlock.proto.Target;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Base64;
import java.util.List;

/** 一台已配对的电脑。 */
final class PcRecord {
    String pcId;
    String phoneId;
    /** 共享密钥 S（32 字节）。 */
    byte[] s;
    String host;
    String user;
    String realname = "";
    List<String> ips = new ArrayList<>();
    int port = Proto.DEFAULT_PORT;
    String bt = "";
    /** 用户填的额外地址，逗号分隔的 host[:port]。 */
    String extra = "";
    long pairedAt;
    /** 测试用的一键解锁：密钥不要求指纹，解锁时点一下按钮就签名。见 KeyHelper.generate。 */
    boolean oneTap;
    /** 离开自动锁屏：0 = 关，否则是蓝牙断开后电脑等待的秒数。由 AwayService 在后台保持蓝牙连接。 */
    int awayGrace;

    static String aliasFor(String pcId) {
        return "hisi-unlock-" + pcId;
    }

    String alias() {
        return aliasFor(pcId);
    }

    String title() {
        return host == null || host.isEmpty() ? "电脑 " + pcId.substring(0, 8) : host;
    }

    List<HostPort> extras() {
        try {
            return HostPort.parseList(extra, port);
        } catch (IllegalArgumentException e) {
            return new ArrayList<>();
        }
    }

    Target target() {
        return new Target(pcId, ips, port, extras(), bt);
    }

    JSONObject toJson() throws JSONException {
        JSONObject o = new JSONObject();
        o.put("pc_id", pcId);
        o.put("phone_id", phoneId);
        o.put("s", Base64.getEncoder().encodeToString(s));
        o.put("host", host);
        o.put("user", user);
        o.put("realname", realname);
        JSONArray a = new JSONArray();
        for (String ip : ips) a.put(ip);
        o.put("ips", a);
        o.put("port", port);
        o.put("bt", bt);
        o.put("extra", extra);
        o.put("paired_at", pairedAt);
        o.put("one_tap", oneTap);
        o.put("away_grace", awayGrace);
        return o;
    }

    static PcRecord fromJson(JSONObject o) throws JSONException {
        PcRecord r = new PcRecord();
        r.pcId = o.getString("pc_id");
        r.phoneId = o.getString("phone_id");
        r.s = Base64.getDecoder().decode(o.getString("s"));
        r.host = o.optString("host", "");
        r.user = o.optString("user", "");
        r.realname = o.optString("realname", "");
        JSONArray a = o.optJSONArray("ips");
        if (a != null) {
            for (int i = 0; i < a.length(); i++) r.ips.add(a.getString(i));
        }
        r.port = o.optInt("port", Proto.DEFAULT_PORT);
        r.bt = o.optString("bt", "");
        r.extra = o.optString("extra", "");
        r.pairedAt = o.optLong("paired_at", 0);
        r.oneTap = o.optBoolean("one_tap", false);
        r.awayGrace = o.optInt("away_grace", 0);
        if (!Proto.validGrace(r.awayGrace)) r.awayGrace = 0;
        return r;
    }
}
