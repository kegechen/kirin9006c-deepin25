package org.hisi.phoneunlock.proto;

import java.io.EOFException;
import java.io.IOException;
import java.net.ConnectException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.NoRouteToHostException;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadFactory;

/**
 * Wi-Fi 连接：同时做 UDP 发现（广播 + 二维码 IP 单播，约 1.2 秒）和直连（二维码 IP、额外地址），
 * 局域网/回环地址的 TCP 连接超时 1.5 秒（远端地址如 Tailscale 放宽，见 isFar）；连上后发 PING 确认是这台电脑
 * （PONG 的 pc_id 不对就丢掉），先到先用。
 */
public final class WifiConnector {
    private final Target target;
    private final TcpTransport.Binder binder;
    private final int discoveryMs;
    private final int connectMs;
    private final int pingMs;

    private final Object lock = new Object();
    private TcpTransport winner;
    private int pending;
    private boolean discovering;
    private boolean finished;
    private int discoveryHits;
    private final Set<String> tried = new HashSet<>();
    private final Set<String> errors = new LinkedHashSet<>();
    private volatile DatagramSocket udp;
    private ExecutorService pool;

    public WifiConnector(Target target, TcpTransport.Binder binder, int discoveryMs, int connectMs, int pingMs) {
        this.target = target;
        this.binder = binder;
        this.discoveryMs = discoveryMs;
        this.connectMs = connectMs;
        this.pingMs = pingMs;
    }

    public WifiConnector(Target target, TcpTransport.Binder binder) {
        this(target, binder, 1200, 1500, 1500);
    }

    public TcpTransport connect() throws IOException, InterruptedException {
        pool = Executors.newCachedThreadPool(new ThreadFactory() {
            int n;

            @Override
            public synchronized Thread newThread(Runnable r) {
                Thread t = new Thread(r, "pu-wifi-" + (++n));
                t.setDaemon(true);
                return t;
            }
        });
        Thread disc = null;
        try {
            for (HostPort hp : target.direct) attempt(hp, "直连");
            synchronized (lock) {
                discovering = true;
            }
            disc = new Thread(new Runnable() {
                @Override
                public void run() {
                    discover();
                }
            }, "pu-discovery");
            disc.setDaemon(true);
            disc.start();

            boolean anyFar = false;
            for (HostPort hp : target.direct) anyFar |= isFar(hp);
            long worst = Math.max(discoveryMs + connectMs + pingMs, anyFar ? farConnectMs() + farPingMs() : 0);
            long hardDeadline = System.currentTimeMillis() + worst + 1500;
            synchronized (lock) {
                while (winner == null && (discovering || pending > 0)) {
                    long left = hardDeadline - System.currentTimeMillis();
                    if (left <= 0) break;
                    lock.wait(left);
                }
                finished = true;
                if (winner != null) {
                    Logger.i("Wi-Fi 连上 " + winner.describe());
                    return winner;
                }
                StringBuilder sb = new StringBuilder();
                if (target.direct.isEmpty() && discoveryHits == 0) sb.append("局域网里没有找到这台电脑");
                else if (discoveryHits == 0) sb.append("局域网广播没有回应");
                for (String e : errors) {
                    if (sb.length() > 0) sb.append("；");
                    sb.append(e);
                }
                throw new IOException(sb.toString());
            }
        } finally {
            synchronized (lock) {
                finished = true;
            }
            DatagramSocket u = udp;
            if (u != null) u.close();
            if (disc != null) disc.interrupt();
            pool.shutdown();
        }
    }

    /**
     * 远端地址：不是局域网、也不是回环的（Tailscale、公网、主机名）。第一次连要先做 WireGuard 握手、可能还要走中继，
     * 1.5 秒的局域网超时不够，放宽到 3 倍（连接）和 2 倍（PING）；它们连不上时只是多等一会儿，不影响先到先用。
     */
    private static boolean isFar(HostPort hp) {
        if (hp.isLoopback()) return false;
        String h = hp.host;
        boolean literal = h.indexOf(':') >= 0 || h.matches("\\d{1,3}(\\.\\d{1,3}){3}");
        if (!literal) return true;
        try {
            return !LanAddress.isLan(InetAddress.getByName(h)); /* 地址字面量，不会查 DNS */
        } catch (UnknownHostException e) {
            return false;
        }
    }

    private int farConnectMs() {
        return connectMs * 3;
    }

    private int farPingMs() {
        return pingMs * 2;
    }

    private void attempt(final HostPort hp, final String via) {
        synchronized (lock) {
            if (winner != null || finished || !tried.add(hp.toString().toLowerCase())) return;
            pending++;
        }
        try {
            pool.execute(new Runnable() {
                @Override
                public void run() {
                    tryOne(hp, via);
                }
            });
        } catch (RejectedExecutionException e) {
            synchronized (lock) {
                pending--;
                lock.notifyAll();
            }
        }
    }

    private void tryOne(HostPort hp, String via) {
        TcpTransport t = null;
        String err = null;
        try {
            boolean far = isFar(hp);
            Logger.i("Wi-Fi 试连 " + hp + "（" + via + (far ? "，远端地址，超时放宽" : "") + "）");
            t = TcpTransport.open(hp, far ? farConnectMs() : connectMs, binder);
            String id = null;
            try {
                id = new Client(t).ping(far ? farPingMs() : pingMs);
            } catch (PeerException pe) {
                /* 回了 ERR 说明是这套协议的电脑，只是不认 PING，照样用（HELLO 会再确认身份） */
                Logger.i(hp + " 不认 PING（ERR " + pe.code + "），继续用");
            }
            if (id != null && !id.equals(target.pcId)) err = hp + " 是另一台电脑";
        } catch (IOException e) {
            err = hp + " " + describe(e);
        } catch (RuntimeException e) {
            err = hp + " " + e;
        }
        synchronized (lock) {
            pending--;
            if (err == null && t != null && winner == null && !finished) {
                winner = t;
                t = null;
            } else if (err != null) {
                Logger.i("Wi-Fi " + err);
                errors.add(err);
            }
            lock.notifyAll();
        }
        if (t != null) t.close();
    }

    private void discover() {
        DatagramSocket ds = null;
        try {
            ds = new DatagramSocket();
            udp = ds;
            if (binder != null) binder.bind(ds);
            ds.setBroadcast(true);
            byte[] probe = Proto.utf8(Proto.discoverLine(target.pcId));
            List<InetAddress> dests = new ArrayList<>();
            dests.add(InetAddress.getByName("255.255.255.255"));
            for (String ip : target.probeIps) {
                try {
                    InetAddress a = InetAddress.getByName(ip);
                    if (!dests.contains(a)) dests.add(a);
                } catch (UnknownHostException ignored) {
                }
            }
            long start = System.currentTimeMillis();
            long end = start + discoveryMs;
            int sends = 0;
            long nextSend = start;
            byte[] rb = new byte[512];
            while (true) {
                long now = System.currentTimeMillis();
                if (now >= end || Thread.currentThread().isInterrupted()) break;
                synchronized (lock) {
                    if (winner != null || finished) break;
                }
                /* UDP 可能丢包：同一个探测包隔 400ms 再发一次 */
                if (sends < 2 && now >= nextSend) {
                    for (InetAddress a : dests) {
                        try {
                            ds.send(new DatagramPacket(probe, probe.length, a, Proto.DISCOVERY_PORT));
                        } catch (IOException e) {
                            if (sends == 0) Logger.i("UDP 发往 " + a.getHostAddress() + " 失败：" + e.getMessage());
                        }
                    }
                    sends++;
                    nextSend = now + 400;
                }
                long wait = end - now;
                if (sends < 2) wait = Math.min(wait, Math.max(1, nextSend - now));
                ds.setSoTimeout((int) Math.max(1, wait));
                DatagramPacket p = new DatagramPacket(rb, rb.length);
                try {
                    ds.receive(p);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                String s = new String(p.getData(), p.getOffset(), p.getLength(), java.nio.charset.StandardCharsets.UTF_8).trim();
                String[] f = s.split(" ");
                if (f.length >= 3 && f[0].equals(Proto.DISCOVER_A) && f[1].equals(target.pcId)) {
                    int port;
                    try {
                        port = Integer.parseInt(f[2]);
                    } catch (NumberFormatException e) {
                        continue;
                    }
                    if (port < 1 || port > 65535) continue;
                    synchronized (lock) {
                        discoveryHits++;
                    }
                    String host = p.getAddress().getHostAddress();
                    Logger.i("UDP 发现电脑 " + host + " 端口 " + port);
                    attempt(new HostPort(host, port), "发现");
                }
            }
        } catch (IOException e) {
            synchronized (lock) {
                if (!finished) errors.add("UDP 发现失败：" + e.getMessage());
            }
        } finally {
            if (ds != null) ds.close();
            synchronized (lock) {
                discovering = false;
                lock.notifyAll();
            }
        }
    }

    public static String describe(IOException e) {
        if (e instanceof SocketTimeoutException) return "连接超时";
        if (e instanceof ConnectException) return "连不上（" + e.getMessage() + "）";
        if (e instanceof NoRouteToHostException) return "网络不通";
        if (e instanceof EOFException) return "连接被对方关闭";
        if (e instanceof UnknownHostException) return "找不到这个主机名";
        String m = e.getMessage();
        return m == null || m.isEmpty() ? e.getClass().getSimpleName() : m;
    }
}
