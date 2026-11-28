package org.hisi.phoneunlock.proto;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;

/** TCP 47390 上的一条会话。 */
public final class TcpTransport implements Transport {
    /** 连接前给 socket 绑网络（Android 上用来强制走 Wi-Fi），可为 null。 */
    public interface Binder {
        void bind(Socket s, InetAddress target) throws IOException;

        void bind(java.net.DatagramSocket s) throws IOException;
    }

    private final Socket sock;
    private final InputStream in;
    private final OutputStream out;
    private final String label;
    private final byte[] buf = new byte[32 * 1024];
    private int pos;
    private int len;

    private TcpTransport(Socket s, String label) throws IOException {
        this.sock = s;
        this.in = s.getInputStream();
        this.out = s.getOutputStream();
        this.label = label;
    }

    public static TcpTransport open(HostPort hp, int connectTimeoutMs, Binder binder) throws IOException {
        InetAddress addr = InetAddress.getByName(hp.host);
        Socket s = new Socket();
        try {
            if (binder != null) binder.bind(s, addr);
            s.connect(new InetSocketAddress(addr, hp.port), connectTimeoutMs);
            s.setTcpNoDelay(true);
            s.setKeepAlive(true);
            return new TcpTransport(s, hp.toString());
        } catch (IOException | RuntimeException e) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
            throw e;
        }
    }

    @Override
    public synchronized void sendLine(String line) throws IOException {
        byte[] b = Proto.utf8(line + "\n");
        if (b.length > Proto.MAX_PHONE_LINE) throw new IOException("要发的消息太长");
        out.write(b);
        out.flush();
    }

    @Override
    public synchronized String readLine(long timeoutMs) throws IOException {
        long deadline = System.currentTimeMillis() + timeoutMs;
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        while (true) {
            for (int i = pos; i < len; i++) {
                if (buf[i] == '\n') {
                    line.write(buf, pos, i - pos);
                    pos = i + 1;
                    return finish(line.toByteArray());
                }
            }
            line.write(buf, pos, len - pos);
            pos = len = 0;
            if (line.size() > Proto.MAX_PC_LINE) throw new IOException("电脑回的消息太长");
            long left = deadline - System.currentTimeMillis();
            if (left <= 0) throw new SocketTimeoutException("等电脑回复超时");
            sock.setSoTimeout((int) Math.max(1, Math.min(left, Integer.MAX_VALUE)));
            int n;
            try {
                n = in.read(buf, 0, buf.length);
            } catch (SocketTimeoutException e) {
                throw new SocketTimeoutException("等电脑回复超时");
            }
            if (n < 0) throw new EOFException("连接被电脑关闭");
            len = n;
        }
    }

    static String finish(byte[] b) {
        int n = b.length;
        if (n > 0 && b[n - 1] == '\r') n--;
        return new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8);
    }

    @Override
    public boolean isBluetooth() {
        return false;
    }

    @Override
    public String describe() {
        return "Wi-Fi " + label;
    }

    @Override
    public void close() {
        try {
            sock.close();
        } catch (IOException ignored) {
        }
    }
}
