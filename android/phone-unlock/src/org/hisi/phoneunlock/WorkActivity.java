package org.hisi.phoneunlock;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.hisi.phoneunlock.proto.Transport;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

/**
 * 需要在后台连电脑的界面的基类：一个后台工作线程 + 让后台线程能“等用户点完权限框 / 打开蓝牙框”。
 */
abstract class WorkActivity extends Activity implements Connector.Host {
    private static final int REQ_PERM = 41;
    private static final int REQ_BT = 42;

    protected final Handler ui = new Handler(Looper.getMainLooper());
    protected final ExecutorService worker = Executors.newSingleThreadExecutor();
    protected volatile boolean destroyed;
    private Future<?> current;

    private CountDownLatch permLatch;
    private CountDownLatch btLatch;

    /** 在后台线程跑（同一时间只排一个任务队列，按顺序执行）。 */
    protected void runBg(final Runnable r) {
        if (destroyed) return;
        current = worker.submit(new Runnable() {
            @Override
            public void run() {
                try {
                    r.run();
                } catch (RuntimeException e) {
                    Log.e(App.TAG, "后台任务出错", e);
                    status("出错了：" + e);
                }
            }
        });
    }

    protected void runUi(Runnable r) {
        if (!destroyed) ui.post(r);
    }

    /** 关连接放到独立线程（BLE 的 close 会等一下断开回调）。 */
    protected static void closeAsync(final Transport t) {
        if (t == null) return;
        new Thread(new Runnable() {
            @Override
            public void run() {
                t.close();
            }
        }, "pu-close").start();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        synchronized (this) {
            if (permLatch != null) permLatch.countDown();
            if (btLatch != null) btLatch.countDown();
        }
        if (current != null) current.cancel(true);
        worker.shutdownNow();
        super.onDestroy();
    }

    /* ==== Connector.Host ==== */

    @Override
    public boolean ensurePermissions(String[] perms) throws InterruptedException {
        final List<String> missing = new ArrayList<>();
        for (String p : perms) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        }
        if (missing.isEmpty()) return true;
        if (destroyed) return false;
        final CountDownLatch latch = new CountDownLatch(1);
        synchronized (this) {
            permLatch = latch;
        }
        Log.i(App.TAG, "申请权限 " + missing);
        ui.post(new Runnable() {
            @Override
            public void run() {
                if (destroyed) {
                    latch.countDown();
                    return;
                }
                requestPermissions(missing.toArray(new String[0]), REQ_PERM);
            }
        });
        latch.await();
        for (String p : perms) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) return false;
        }
        return true;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_PERM) {
            synchronized (this) {
                if (permLatch != null) permLatch.countDown();
                permLatch = null;
            }
        }
    }

    @Override
    public boolean ensureBluetoothOn() throws InterruptedException {
        final BluetoothManager bm = getSystemService(BluetoothManager.class);
        final BluetoothAdapter ad = bm == null ? null : bm.getAdapter();
        if (ad == null) return false;
        if (ad.isEnabled()) return true;
        if (destroyed) return false;
        status("请允许打开蓝牙…");
        final CountDownLatch latch = new CountDownLatch(1);
        btAllowed = false;
        synchronized (this) {
            btLatch = latch;
        }
        ui.post(new Runnable() {
            @Override
            public void run() {
                if (destroyed) {
                    latch.countDown();
                    return;
                }
                try {
                    startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_BT);
                } catch (ActivityNotFoundException | SecurityException e) {
                    Log.w(App.TAG, "没法请求打开蓝牙：" + e);
                    latch.countDown();
                }
            }
        });
        latch.await();
        if (!btAllowed) return ad.isEnabled();
        /* 用户点了允许之后蓝牙还要一两秒才真正打开 */
        for (int i = 0; i < 50 && !ad.isEnabled() && !destroyed; i++) Thread.sleep(100);
        return ad.isEnabled();
    }

    private volatile boolean btAllowed;

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_BT) {
            btAllowed = resultCode == RESULT_OK;
            synchronized (this) {
                if (btLatch != null) btLatch.countDown();
                btLatch = null;
            }
        }
    }
}
