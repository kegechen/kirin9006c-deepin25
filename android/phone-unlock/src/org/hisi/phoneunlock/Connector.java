package org.hisi.phoneunlock;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.util.Log;

import org.hisi.phoneunlock.proto.LanAddress;
import org.hisi.phoneunlock.proto.Target;
import org.hisi.phoneunlock.proto.TcpTransport;
import org.hisi.phoneunlock.proto.Transport;
import org.hisi.phoneunlock.proto.WifiConnector;

import java.io.IOException;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.Socket;

/** 按连接方式（自动 / Wi-Fi / 蓝牙）连上电脑。必须在后台线程调用。 */
final class Connector {

    /** 由界面实现：显示进度、按需申请运行时权限、请求打开蓝牙。后两个会阻塞调用线程直到用户选完。 */
    interface Host {
        void status(String text);

        boolean ensurePermissions(String[] perms) throws InterruptedException;

        boolean ensureBluetoothOn() throws InterruptedException;
    }

    private final Context ctx;
    private final Host host;

    Connector(Context ctx, Host host) {
        this.ctx = ctx.getApplicationContext();
        this.host = host;
    }

    Transport connect(Target t, String mode) throws IOException, InterruptedException {
        Log.i(App.TAG, "连接方式 " + mode);
        if (Store.MODE_WIFI.equals(mode)) return wifi(t);
        if (Store.MODE_BT.equals(mode)) return bluetooth(t);
        IOException wifiErr;
        try {
            return wifi(t);
        } catch (IOException e) {
            wifiErr = e;
            Log.i(App.TAG, "Wi-Fi 不通，改用蓝牙：" + e.getMessage());
        }
        host.status("Wi-Fi 没连上，改用蓝牙…");
        try {
            return bluetooth(t);
        } catch (IOException e) {
            throw new IOException("Wi-Fi：" + wifiErr.getMessage() + "\n蓝牙：" + e.getMessage());
        }
    }

    private Transport wifi(Target t) throws IOException, InterruptedException {
        host.status("正在通过 Wi-Fi 寻找电脑…");
        return new WifiConnector(t, new WifiBinder(ctx)).connect();
    }

    private Transport bluetooth(Target t) throws IOException, InterruptedException {
        host.status("正在通过蓝牙连接电脑…");
        BluetoothManager bm = ctx.getSystemService(BluetoothManager.class);
        BluetoothAdapter ad = bm == null ? null : bm.getAdapter();
        if (ad == null) throw new IOException("这部手机没有蓝牙");
        if (Build.VERSION.SDK_INT >= 31
                && !host.ensurePermissions(new String[] {Manifest.permission.BLUETOOTH_CONNECT})) {
            throw new IOException("没有“附近的设备”（蓝牙连接）权限");
        }
        if (!ad.isEnabled() && !host.ensureBluetoothOn()) throw new IOException("手机蓝牙没有打开");
        try {
            return BleTransport.open(ctx, ad, t, host);
        } catch (SecurityException e) {
            throw new IOException("缺少蓝牙权限：" + e.getMessage());
        }
    }

    /**
     * 当默认网络不是 Wi-Fi（例如连的 Wi-Fi 没有互联网，系统改走了移动数据）时，把 socket 绑到 Wi-Fi 上，
     * 这样局域网地址才走得通。只绑局域网地址：回环地址（adb reverse 调试用的 127.0.0.1）、Tailscale 地址（100.64.0.0/10）、
     * 公网地址都不绑，按系统默认路由走（Tailscale 的 VPN、移动数据），硬绑到 Wi-Fi 上反而不通。
     */
    static final class WifiBinder implements TcpTransport.Binder {
        private final Network wifi;

        @SuppressWarnings("deprecation")
        WifiBinder(Context ctx) {
            Network found = null;
            try {
                ConnectivityManager cm = ctx.getSystemService(ConnectivityManager.class);
                Network active = cm.getActiveNetwork();
                NetworkCapabilities ac = active == null ? null : cm.getNetworkCapabilities(active);
                if (ac == null || !ac.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                    for (Network n : cm.getAllNetworks()) {
                        NetworkCapabilities nc = cm.getNetworkCapabilities(n);
                        if (nc != null && nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                            found = n;
                            break;
                        }
                    }
                }
            } catch (RuntimeException e) {
                Log.w(App.TAG, "查 Wi-Fi 网络失败：" + e);
            }
            wifi = found;
            if (wifi != null) Log.i(App.TAG, "默认网络不是 Wi-Fi，局域网连接绑到 Wi-Fi 上");
        }

        @Override
        public void bind(Socket s, InetAddress target) throws IOException {
            if (wifi != null && LanAddress.isLan(target)) wifi.bindSocket(s);
        }

        @Override
        public void bind(DatagramSocket s) throws IOException {
            if (wifi != null) wifi.bindSocket(s);
        }
    }
}
