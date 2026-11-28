package org.hisi.phoneunlock;

/**
 * 解锁界面和后台服务（离开自动锁屏）共用同一条蓝牙链路时，两边不能同时说话：
 * 电脑那边同一个蓝牙设备只有一个会话，挑战也只记最新一个——后台服务在解锁界面 HELLO 和 UNLOCK 之间插一次 HELLO，
 * 解锁就会被当成签名错。所以解锁界面拿着蓝牙连接的整个期间，后台服务都不发请求；
 * 后台服务正在收发时，解锁界面先等它发完。
 */
final class LinkGate {
    private static int ui;
    private static boolean bg;

    private LinkGate() {
    }

    /**
     * 解锁界面开始用蓝牙连接：后台服务正在收发就等它完；另一个解锁界面（正在关掉的旧实例）占着也等它放手。
     * 同一时间只有一个解锁界面（UnlockActivity 开新的时会关掉旧的），所以不会一直等。
     */
    static synchronized void uiEnter() throws InterruptedException {
        while (bg || ui > 0) LinkGate.class.wait();
        ui++;
    }

    static synchronized void uiLeave() {
        if (ui > 0) ui--;
        LinkGate.class.notifyAll();
    }

    /** 后台服务想收发：解锁界面正用着、或后台别的线程正在收发，就返回 false（过会儿再试）。 */
    static synchronized boolean bgEnter() {
        if (ui > 0 || bg) return false;
        bg = true;
        return true;
    }

    static synchronized void bgLeave() {
        bg = false;
        LinkGate.class.notifyAll();
    }
}
