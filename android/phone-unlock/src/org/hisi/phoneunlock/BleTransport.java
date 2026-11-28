package org.hisi.phoneunlock;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.location.LocationManager;
import android.os.Build;
import android.os.ParcelUuid;
import android.util.Log;

import org.hisi.phoneunlock.proto.Client;
import org.hisi.phoneunlock.proto.PeerException;
import org.hisi.phoneunlock.proto.Proto;
import org.hisi.phoneunlock.proto.Target;
import org.hisi.phoneunlock.proto.Transport;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.SocketTimeoutException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;

/**
 * BLE GATT 会话：电脑是外设。requestMtu(517) → discoverServices → 写 TX 的 CCCD 开通知 →
 * 按 MTU-3 分片写 RX（每片等 onCharacteristicWrite 再写下一片）→ 拼通知直到 \n。
 */
@SuppressWarnings({"deprecation", "MissingPermission"})
final class BleTransport extends BluetoothGattCallback implements Transport {
    static final UUID SERVICE = UUID.fromString(Proto.BLE_SERVICE);
    static final UUID RX = UUID.fromString(Proto.BLE_RX);
    static final UUID TX = UUID.fromString(Proto.BLE_TX);
    static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    /**
     * 建立连接（含扫描回退）的整体超时。
     * 电脑是 5.4 内核时，扩展广播的间隔写死 1.28 秒，而且广播数据发不到 BlueZ 的广播实例上
     * （空广播，按服务 UUID 扫不到，只能按蓝牙地址连），手机连上要 1～6 秒，所以给足时间。
     */
    static final long TOTAL_TIMEOUT_MS = 20000;
    /** 直连蓝牙地址时“连上”这一步最多等这么久，不行就改扫描。 */
    static final long DIRECT_TIMEOUT_MS = 12000;
    /** 连上之后协商 MTU、找服务、开通知至少留这么久（不和连接共用倒计时）。 */
    static final long GATT_SETUP_MS = 6000;

    private final Context ctx;
    private final BluetoothDevice dev;
    private final Object lock = new Object();
    /** 后台服务用：交给系统在设备回到范围内时自动连（不限时、省电）。 */
    private boolean autoConnect;
    private boolean kicked;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rx;
    private boolean connected;
    private boolean disconnected;
    private int connStatus;
    private int mtu = 23;
    private boolean mtuDone;
    private boolean servicesDone;
    private int servicesStatus;
    private boolean descDone;
    private int descStatus;
    private boolean writeDone;
    private int writeStatus;
    private boolean overflow;
    private final ByteArrayOutputStream acc = new ByteArrayOutputStream();
    private final ArrayDeque<String> lines = new ArrayDeque<>();

    private BleTransport(Context ctx, BluetoothDevice dev) {
        this.ctx = ctx;
        this.dev = dev;
    }

    /* ==== 建立连接 ==== */

    static Transport open(Context ctx, BluetoothAdapter ad, Target t, Connector.Host host)
            throws IOException, InterruptedException {
        long deadline = System.currentTimeMillis() + TOTAL_TIMEOUT_MS;
        String lastErr;
        if (!t.bt.isEmpty() && BluetoothAdapter.checkBluetoothAddress(t.bt)) {
            host.status("正在连接电脑蓝牙 " + t.bt + "…");
            BleTransport b = new BleTransport(ctx, ad.getRemoteDevice(t.bt));
            try {
                b.setup(Math.min(deadline, System.currentTimeMillis() + DIRECT_TIMEOUT_MS), deadline);
                return b;
            } catch (IOException e) {
                b.close();
                lastErr = "直连 " + t.bt + " 失败：" + e.getMessage();
                Log.i(App.TAG, lastErr);
            }
        } else {
            lastErr = "二维码里没有蓝牙地址";
        }

        /* 扫描回退：按需申请权限（等用户点权限框的时间不算进超时） */
        long t0 = System.currentTimeMillis();
        String why = scanPermissionProblem(ctx, host);
        deadline += System.currentTimeMillis() - t0;
        if (why != null) throw new IOException(lastErr + "；" + why);

        Set<String> skip = new HashSet<>();
        while (System.currentTimeMillis() < deadline - 500) {
            host.status("正在扫描附近的电脑（蓝牙）…");
            BluetoothDevice d = scanOne(ad, skip, t.bt, deadline);
            if (d == null) {
                lastErr += "；附近没扫描到电脑";
                break;
            }
            skip.add(d.getAddress());
            host.status("找到 " + d.getAddress() + "，正在连接…");
            BleTransport b = new BleTransport(ctx, d);
            try {
                b.setup(deadline, deadline);
                if (!d.getAddress().equalsIgnoreCase(t.bt)) {
                    /* 扫到的不是二维码里的地址：PING 确认是不是这台电脑 */
                    String id = null;
                    try {
                        id = new Client(b).ping(Math.max(1500, deadline - System.currentTimeMillis()));
                    } catch (PeerException pe) {
                        Log.i(App.TAG, d.getAddress() + " 不认 PING（ERR " + pe.code + "），继续用");
                    }
                    if (id != null && !id.equals(t.pcId)) {
                        b.close();
                        lastErr += "；" + d.getAddress() + " 是另一台电脑";
                        continue;
                    }
                }
                return b;
            } catch (IOException e) {
                b.close();
                lastErr += "；" + d.getAddress() + " " + e.getMessage();
            }
        }
        throw new IOException(lastErr);
    }

    /**
     * 后台服务（离开自动锁屏）用：只按蓝牙地址连，不扫描、不申请权限。先 background() 再 connectBy()，
     * 中间别的线程可以 abort() 掉。autoConnect=false 是直连；true 交给系统后台自动连（电脑回到范围内才连上）。
     */
    static BleTransport background(Context ctx, BluetoothAdapter ad, String bt, boolean autoConnect) {
        BleTransport b = new BleTransport(ctx, ad.getRemoteDevice(bt));
        b.autoConnect = autoConnect;
        return b;
    }

    /** connectBy 前没连上（或被 abort）抛异常；连上后协商 / 找服务另给 GATT_SETUP_MS。失败后调用者负责 close()。 */
    void connectBy(long connectBy) throws IOException {
        setup(connectBy, 0);
    }

    /** 让正在进行的连接 / 等待马上以“断开”结束（别的线程调）。 */
    void abort() {
        synchronized (lock) {
            disconnected = true;
            lock.notifyAll();
        }
    }

    /** 叫醒 waitDisconnect（别的线程调）。 */
    void kick() {
        synchronized (lock) {
            kicked = true;
            lock.notifyAll();
        }
    }

    /** 扫描需要的权限 / 开关；没问题返回 null。 */
    private static String scanPermissionProblem(Context ctx, Connector.Host host) throws InterruptedException {
        if (Build.VERSION.SDK_INT >= 31) {
            if (!host.ensurePermissions(new String[] {Manifest.permission.BLUETOOTH_SCAN})) {
                return "没有“附近的设备”（蓝牙扫描）权限，没法扫描";
            }
            return null;
        }
        if (!host.ensurePermissions(new String[] {Manifest.permission.ACCESS_FINE_LOCATION})) {
            return "没有定位权限（Android 11 及以下扫描蓝牙需要），没法扫描";
        }
        LocationManager lm = ctx.getSystemService(LocationManager.class);
        if (lm != null && !lm.isLocationEnabled()) return "手机的定位开关没打开（扫描蓝牙需要），没法扫描";
        return null;
    }

    /** 扫一台：带电脑解锁服务 UUID 的，或者地址就是已知蓝牙地址的（有的电脑广播里没数据，只能认地址）。 */
    private static BluetoothDevice scanOne(BluetoothAdapter ad, final Set<String> skip, String knownBt, long deadline)
            throws InterruptedException, IOException {
        BluetoothLeScanner sc = ad.getBluetoothLeScanner();
        if (sc == null) throw new IOException("蓝牙扫描不可用");
        final Object l = new Object();
        final BluetoothDevice[] found = {null};
        final int[] fail = {0};
        ScanCallback cb = new ScanCallback() {
            @Override
            public void onScanResult(int callbackType, ScanResult r) {
                BluetoothDevice d = r.getDevice();
                synchronized (l) {
                    if (found[0] == null && d != null && !skip.contains(d.getAddress())) {
                        Log.i(App.TAG, "扫描到 " + d.getAddress() + " rssi=" + r.getRssi());
                        found[0] = d;
                        l.notifyAll();
                    }
                }
            }

            @Override
            public void onBatchScanResults(List<ScanResult> results) {
                for (ScanResult r : results) onScanResult(0, r);
            }

            @Override
            public void onScanFailed(int errorCode) {
                synchronized (l) {
                    fail[0] = errorCode;
                    l.notifyAll();
                }
            }
        };
        List<ScanFilter> filters = new ArrayList<>();
        filters.add(new ScanFilter.Builder().setServiceUuid(new ParcelUuid(SERVICE)).build());
        if (knownBt != null && BluetoothAdapter.checkBluetoothAddress(knownBt) && !skip.contains(knownBt)) {
            filters.add(new ScanFilter.Builder().setDeviceAddress(knownBt).build());
        }
        ScanSettings st = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        sc.startScan(filters, st, cb);
        try {
            synchronized (l) {
                while (found[0] == null && fail[0] == 0) {
                    long left = deadline - System.currentTimeMillis();
                    if (left <= 0) break;
                    l.wait(left);
                }
            }
        } finally {
            try {
                sc.stopScan(cb);
            } catch (RuntimeException ignored) {
            }
        }
        if (found[0] == null && fail[0] != 0) throw new IOException("蓝牙扫描失败（代码 " + fail[0] + "）");
        return found[0];
    }

    private interface Cond {
        boolean ok();
    }

    /** 在 lock 上等条件成立；断开或超时抛异常。调用前不要持有 lock。 */
    private void await(Cond c, long deadline, String timeoutMsg) throws IOException {
        synchronized (lock) {
            while (!c.ok()) {
                if (disconnected) throw new EOFException("蓝牙连接断开了（status " + connStatus + "）");
                long left = deadline - System.currentTimeMillis();
                if (left <= 0) throw new SocketTimeoutException(timeoutMsg);
                try {
                    lock.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new InterruptedIOException("已取消");
                }
            }
        }
    }

    /** connectBy：等“连上”的截止时刻；deadline：整体截止时刻，连上后至少再留 GATT_SETUP_MS。 */
    private void setup(long connectBy, long deadline) throws IOException {
        BluetoothGatt g = dev.connectGatt(ctx, autoConnect, this, BluetoothDevice.TRANSPORT_LE);
        if (g == null) throw new IOException("connectGatt 失败");
        synchronized (lock) {
            gatt = g;
        }
        long t0 = System.currentTimeMillis();
        await(new Cond() {
            @Override
            public boolean ok() {
                return connected;
            }
        }, connectBy, "蓝牙连接超时");
        Log.i(App.TAG, "蓝牙已连接 " + dev.getAddress() + "（" + (System.currentTimeMillis() - t0) + " ms）");
        deadline = Math.max(deadline, System.currentTimeMillis() + GATT_SETUP_MS);

        if (g.requestMtu(517)) {
            try {
                await(new Cond() {
                    @Override
                    public boolean ok() {
                        return mtuDone;
                    }
                }, Math.min(deadline, System.currentTimeMillis() + 3000), "MTU 协商超时");
            } catch (SocketTimeoutException e) {
                Log.i(App.TAG, "MTU 协商没有回应，按 " + mtu + " 用");
            }
        }

        if (!g.discoverServices()) throw new IOException("没法开始查找蓝牙服务");
        await(new Cond() {
            @Override
            public boolean ok() {
                return servicesDone;
            }
        }, deadline, "查找蓝牙服务超时");
        if (servicesStatus != BluetoothGatt.GATT_SUCCESS) throw new IOException("查找蓝牙服务失败（status " + servicesStatus + "）");
        BluetoothGattService svc = g.getService(SERVICE);
        if (svc == null) throw new IOException("这台蓝牙设备上没有电脑解锁服务");
        BluetoothGattCharacteristic rxc = svc.getCharacteristic(RX);
        BluetoothGattCharacteristic txc = svc.getCharacteristic(TX);
        if (rxc == null || txc == null) throw new IOException("电脑解锁服务缺少收发特征");
        rx = rxc;

        if (!g.setCharacteristicNotification(txc, true)) throw new IOException("打开蓝牙通知失败");
        BluetoothGattDescriptor d = txc.getDescriptor(CCCD);
        if (d == null) throw new IOException("TX 特征没有 CCCD");
        synchronized (lock) {
            descDone = false;
        }
        boolean started;
        if (Build.VERSION.SDK_INT >= 33) {
            started = g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS;
        } else {
            d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            started = g.writeDescriptor(d);
        }
        if (!started) throw new IOException("写 CCCD 失败");
        await(new Cond() {
            @Override
            public boolean ok() {
                return descDone;
            }
        }, deadline, "打开蓝牙通知超时");
        if (descStatus != BluetoothGatt.GATT_SUCCESS) throw new IOException("打开蓝牙通知失败（status " + descStatus + "）");
        Log.i(App.TAG, "蓝牙会话就绪，MTU " + mtu);
    }

    /* ==== Transport ==== */

    @Override
    public void sendLine(String line) throws IOException {
        byte[] data = Proto.utf8(line + "\n");
        if (data.length > Proto.MAX_PHONE_LINE) throw new IOException("要发的消息太长");
        int chunk;
        synchronized (lock) {
            chunk = Math.min(Math.max(mtu - 3, 20), 512);
        }
        for (int off = 0; off < data.length; off += chunk) {
            writeChunk(Arrays.copyOfRange(data, off, Math.min(data.length, off + chunk)));
        }
    }

    private void writeChunk(byte[] part) throws IOException {
        BluetoothGatt g;
        synchronized (lock) {
            g = gatt;
            writeDone = false;
        }
        if (g == null || rx == null) throw new EOFException("蓝牙连接已关闭");
        long deadline = System.currentTimeMillis() + 5000;
        boolean started = false;
        for (int i = 0; i < 25 && !started; i++) {
            if (Build.VERSION.SDK_INT >= 33) {
                started = g.writeCharacteristic(rx, part, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                        == BluetoothStatusCodes.SUCCESS;
            } else {
                rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                rx.setValue(part);
                started = g.writeCharacteristic(rx);
            }
            if (!started) {
                synchronized (lock) {
                    if (disconnected) throw new EOFException("蓝牙连接断开了");
                }
                try {
                    Thread.sleep(20); /* 协议栈忙，稍等再试 */
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new InterruptedIOException("已取消");
                }
            }
        }
        if (!started) throw new IOException("蓝牙写入失败");
        await(new Cond() {
            @Override
            public boolean ok() {
                return writeDone;
            }
        }, deadline, "蓝牙写入超时");
        if (writeStatus != BluetoothGatt.GATT_SUCCESS) throw new IOException("蓝牙写入出错（status " + writeStatus + "）");
    }

    @Override
    public String readLine(long timeoutMs) throws IOException {
        long deadline = System.currentTimeMillis() + timeoutMs;
        synchronized (lock) {
            while (lines.isEmpty()) {
                if (overflow) throw new IOException("电脑回的消息太长");
                if (disconnected) throw new EOFException("蓝牙连接断开了");
                long left = deadline - System.currentTimeMillis();
                if (left <= 0) throw new SocketTimeoutException("等电脑回复超时");
                try {
                    lock.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new InterruptedIOException("已取消");
                }
            }
            return lines.poll();
        }
    }

    @Override
    public void drain() {
        synchronized (lock) {
            if (!lines.isEmpty()) Log.i(App.TAG, "丢掉 " + lines.size() + " 条不是给自己的蓝牙回复");
            lines.clear();
            acc.reset();
            overflow = false;
        }
    }

    /** 等蓝牙断开：断开了返回 true；到时间或被 kick() 叫醒时还连着返回 false。 */
    boolean waitDisconnect(long ms) throws InterruptedException {
        long end = System.currentTimeMillis() + ms;
        synchronized (lock) {
            while (!disconnected && !kicked) {
                long left = end - System.currentTimeMillis();
                if (left <= 0) return false;
                lock.wait(left);
            }
            kicked = false;
            return disconnected;
        }
    }

    boolean isDisconnected() {
        synchronized (lock) {
            return disconnected;
        }
    }

    /** 请求低功耗连接参数（后台一直连着时用）。 */
    void lowPower() {
        BluetoothGatt g;
        synchronized (lock) {
            g = gatt;
        }
        try {
            if (g != null) g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_LOW_POWER);
        } catch (RuntimeException e) {
            Log.w(App.TAG, "请求低功耗连接参数失败：" + e);
        }
    }

    @Override
    public boolean isBluetooth() {
        return true;
    }

    @Override
    public String describe() {
        return "蓝牙 " + dev.getAddress();
    }

    /** disconnect() + close()。会等最多 0.5 秒断开回调，别在主线程调。 */
    @Override
    public void close() {
        BluetoothGatt g;
        synchronized (lock) {
            g = gatt;
            gatt = null;
        }
        if (g == null) return;
        try {
            g.disconnect();
            synchronized (lock) {
                long end = System.currentTimeMillis() + 500;
                while (!disconnected && connected) {
                    long left = end - System.currentTimeMillis();
                    if (left <= 0) break;
                    lock.wait(left);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (RuntimeException e) {
            Log.w(App.TAG, "蓝牙 disconnect 出错：" + e);
        }
        try {
            g.close();
        } catch (RuntimeException e) {
            Log.w(App.TAG, "蓝牙 close 出错：" + e);
        }
        Log.i(App.TAG, "蓝牙连接已关闭 " + dev.getAddress());
    }

    /* ==== BluetoothGattCallback（在 binder 线程上） ==== */

    @Override
    public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
        synchronized (lock) {
            connStatus = status;
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                connected = true;
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS) {
                disconnected = true;
            }
            lock.notifyAll();
        }
    }

    @Override
    public void onMtuChanged(BluetoothGatt g, int newMtu, int status) {
        synchronized (lock) {
            if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu;
            mtuDone = true;
            lock.notifyAll();
        }
    }

    @Override
    public void onServicesDiscovered(BluetoothGatt g, int status) {
        synchronized (lock) {
            servicesStatus = status;
            servicesDone = true;
            lock.notifyAll();
        }
    }

    @Override
    public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
        synchronized (lock) {
            descStatus = status;
            descDone = true;
            lock.notifyAll();
        }
    }

    @Override
    public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
        synchronized (lock) {
            writeStatus = status;
            writeDone = true;
            lock.notifyAll();
        }
    }

    /** Android 13+ 走这个（不调 super，所以旧回调不会再来一次）。 */
    @Override
    public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
        if (TX.equals(c.getUuid())) onData(value);
    }

    /** Android 12 及以下走这个。 */
    @Override
    public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
        if (TX.equals(c.getUuid())) onData(c.getValue());
    }

    private void onData(byte[] v) {
        if (v == null) return;
        synchronized (lock) {
            int start = 0;
            for (int i = 0; i < v.length; i++) {
                if (v[i] == '\n') {
                    acc.write(v, start, i - start);
                    byte[] b = acc.toByteArray();
                    int n = b.length;
                    if (n > 0 && b[n - 1] == '\r') n--;
                    lines.add(new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8));
                    acc.reset();
                    start = i + 1;
                }
            }
            acc.write(v, start, v.length - start);
            if (acc.size() > Proto.MAX_PC_LINE + 1024) {
                overflow = true;
                acc.reset();
            }
            lock.notifyAll();
        }
    }
}
