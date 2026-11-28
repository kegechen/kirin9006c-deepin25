package org.hisi.phoneunlock.proto;

/** 极简日志出口：JVM 上打到 stderr，Android 上由 App 换成 android.util.Log。不要往这里写密钥、配对码、签名。 */
public final class Logger {
    private Logger() {}

    public interface Sink {
        void log(String msg);
    }

    private static volatile Sink sink = new Sink() {
        @Override
        public void log(String msg) {
            System.err.println("[PhoneUnlock] " + msg);
        }
    };

    public static void setSink(Sink s) {
        sink = s;
    }

    public static void i(String msg) {
        Sink s = sink;
        if (s != null) s.log(msg);
    }
}
