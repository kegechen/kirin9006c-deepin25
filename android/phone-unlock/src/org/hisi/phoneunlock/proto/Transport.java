package org.hisi.phoneunlock.proto;

import java.io.Closeable;
import java.io.IOException;

/** 一条会话通道（TCP 连接或 BLE 连接），收发以 \n 结尾的文本行。 */
public interface Transport extends Closeable {
    /** 发一行；line 不含 \n，实现负责补上。 */
    void sendLine(String line) throws IOException;

    /** 读一行（不含 \n）。超时抛 SocketTimeoutException，对方断开抛 EOFException。 */
    String readLine(long timeoutMs) throws IOException;

    /**
     * 丢掉已经收到、还没读的行。蓝牙链路被解锁界面和后台服务（离开自动锁屏）共用时，
     * 电脑的通知两边都会收到，开始一次请求前先清掉别人的回复。
     */
    default void drain() {
    }

    boolean isBluetooth();

    /** 给人看的连接说明，例如 "Wi-Fi 192.168.1.5:47390"。 */
    String describe();

    @Override
    void close();
}
