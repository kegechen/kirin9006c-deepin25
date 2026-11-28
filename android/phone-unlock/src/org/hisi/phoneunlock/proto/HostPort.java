package org.hisi.phoneunlock.proto;

import java.util.ArrayList;
import java.util.List;

/** host[:port]，IPv6 写成 [addr]:port。 */
public final class HostPort {
    public final String host;
    public final int port;

    public HostPort(String host, int port) {
        this.host = host;
        this.port = port;
    }

    public static HostPort parse(String s, int defPort) {
        String t = s.trim();
        if (t.isEmpty()) throw new IllegalArgumentException("地址为空");
        String host;
        String portStr = null;
        if (t.startsWith("[")) {
            int close = t.indexOf(']');
            if (close < 0) throw new IllegalArgumentException("地址格式不对：" + t);
            host = t.substring(1, close);
            String rest = t.substring(close + 1);
            if (rest.startsWith(":")) portStr = rest.substring(1);
            else if (!rest.isEmpty()) throw new IllegalArgumentException("地址格式不对：" + t);
        } else {
            int c = t.indexOf(':');
            if (c >= 0 && t.indexOf(':', c + 1) < 0) {
                host = t.substring(0, c);
                portStr = t.substring(c + 1);
            } else {
                host = t; /* 无端口，或裸 IPv6 */
            }
        }
        if (host.isEmpty()) throw new IllegalArgumentException("地址格式不对：" + t);
        for (int i = 0; i < host.length(); i++) {
            char ch = host.charAt(i);
            if (Character.isWhitespace(ch) || ch == '/' || ch == '?' || ch == '#') {
                throw new IllegalArgumentException("地址格式不对：" + t);
            }
        }
        int port = defPort;
        if (portStr != null) {
            try {
                port = Integer.parseInt(portStr);
            } catch (NumberFormatException e) {
                throw new IllegalArgumentException("端口不对：" + t);
            }
            if (port < 1 || port > 65535) throw new IllegalArgumentException("端口不对：" + t);
        }
        return new HostPort(host, port);
    }

    /** 逗号分隔（也接受中文逗号、分号、空白）。 */
    public static List<HostPort> parseList(String csv, int defPort) {
        List<HostPort> out = new ArrayList<>();
        if (csv == null) return out;
        for (String part : csv.split("[,，;；\\s]+")) {
            if (part.trim().isEmpty()) continue;
            HostPort hp = parse(part, defPort);
            if (!out.contains(hp)) out.add(hp);
        }
        return out;
    }

    public boolean isLoopback() {
        return host.equals("localhost") || host.startsWith("127.") || host.equals("::1");
    }

    @Override
    public String toString() {
        return host.indexOf(':') >= 0 ? "[" + host + "]:" + port : host + ":" + port;
    }

    @Override
    public boolean equals(Object o) {
        if (!(o instanceof HostPort)) return false;
        HostPort h = (HostPort) o;
        return h.port == port && h.host.equalsIgnoreCase(host);
    }

    @Override
    public int hashCode() {
        return host.toLowerCase().hashCode() * 31 + port;
    }
}
