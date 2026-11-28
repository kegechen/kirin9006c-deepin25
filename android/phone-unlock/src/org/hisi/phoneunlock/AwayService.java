package org.hisi.phoneunlock;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import org.hisi.phoneunlock.proto.Client;
import org.hisi.phoneunlock.proto.PeerException;
import org.hisi.phoneunlock.proto.Proto;
import org.hisi.phoneunlock.proto.WrongPcException;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * 离开自动锁屏（类似 Windows 的动态锁）：前台服务，给每台开了这一项的电脑保持一条蓝牙连接，
 * 连上就发 HELLO + AWAY &lt;秒数&gt;。之后手机走远、蓝牙断开，由电脑自己倒计时锁屏，这里什么都不用做。
 * 连接：先直连 DIRECT_MS（多半只是闪断，或者电脑就在旁边），连不上交给系统 autoConnect，等电脑回到附近。
 * 连着时每 REFRESH_MS 重发一次 HELLO + AWAY（电脑上 hisi-bio-unlock 重启会忘掉这个开关）。
 * 关掉时先发 AWAY 0 再断开，不然电脑会把这次断开当成离开、锁屏。
 * 和解锁界面共用同一条蓝牙链路，收发用 LinkGate 错开。
 */
public final class AwayService extends Service {
    private static final String CHANNEL = "away";
    private static final int NOTE_ID = 1;

    static final long DIRECT_MS = 20_000;
    static final long REFRESH_MS = 5 * 60_000L;
    /** 请求失败（没断开）、自动连接马上失败、手机蓝牙关着时，过这么久再试。 */
    static final long RETRY_MS = 30_000;
    /** 电脑拒绝（没配对、版本旧等）后过这么久再试。 */
    static final long PEER_RETRY_MS = 5 * 60_000L;
    /** 解锁界面正用着蓝牙：过这么久再试。 */
    static final long BUSY_RETRY_MS = 10_000;
    /** 关掉时最多等解锁界面这么久，好把 AWAY 0 发出去。 */
    static final long OFF_WAIT_MS = 15_000;
    static final long READ_MS = 15_000;
    /** 连着但请求连续失败这么多次：断开重连。 */
    static final int MAX_FAILS = 3;

    /** pcId → 给人看的状态（卡片和通知上显示）。 */
    private static final Map<String, String> states = new ConcurrentHashMap<>();
    private static Runnable listener;          /* 主线程 */
    private static AwayService instance;       /* 主线程 */

    private final Handler main = new Handler(Looper.getMainLooper());
    /** 在跑的（主线程）。 */
    private final Map<String, Worker> workers = new HashMap<>();
    /** 关掉了、正在发 AWAY 0 收尾的（主线程）。 */
    private final List<Worker> ending = new ArrayList<>();

    /**
     * 手机蓝牙开关：一打开就叫醒在等的线程马上连（不然要等 RETRY_MS，关蓝牙再打开时可能赶不上电脑的倒计时）；
     * 关掉就放弃正在进行的连接（系统蓝牙重启后旧的 autoConnect 不会再回调，留着会一直卡住）。
     */
    private final BroadcastReceiver btReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context c, Intent i) {
            int st = i.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1);
            if (st != BluetoothAdapter.STATE_ON && st != BluetoothAdapter.STATE_TURNING_OFF) return;
            Log.i(App.TAG, "[离开锁屏] 手机蓝牙" + (st == BluetoothAdapter.STATE_ON ? "打开了" : "正在关闭"));
            for (Worker w : workers.values()) w.btChanged(st == BluetoothAdapter.STATE_ON);
        }
    };

    /* ==== 给界面用（都在主线程调） ==== */

    static boolean enabled(PcRecord r) {
        return r.awayGrace > 0 && BluetoothAdapter.checkBluetoothAddress(r.bt);
    }

    /** 按配对记录启动 / 更新 / 停掉。 */
    static void sync(Context ctx) {
        if (instance != null) {
            instance.resync();
            return;
        }
        boolean any = false;
        for (PcRecord r : Store.list(ctx)) any |= enabled(r);
        if (!any) return;
        Context app = ctx.getApplicationContext();
        try {
            app.startForegroundService(new Intent(app, AwayService.class));
        } catch (RuntimeException e) {
            /* Android 12+ 不让后台启动前台服务：等下次打开 App */
            Log.w(App.TAG, "启动离开自动锁屏服务失败：" + e);
        }
    }

    static String state(String pcId) {
        String s = states.get(pcId);
        return s == null ? "" : s;
    }

    static void setListener(Runnable r) {
        listener = r;
    }

    /* ==== Service ==== */

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "离开自动锁屏", NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("手机和电脑保持蓝牙连接；手机走远后电脑自动锁屏");
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
        IntentFilter f = new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(btReceiver, f, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(btReceiver, f);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(NOTE_ID, note());          /* 5 秒内必须调，哪怕马上又要停 */
        resync();
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        if (instance == this) instance = null;
        unregisterReceiver(btReceiver);
        for (Worker w : workers.values()) w.finish();
        workers.clear();
        super.onDestroy();
    }

    private void resync() {
        Map<String, PcRecord> want = new HashMap<>();
        for (PcRecord r : Store.list(this)) {
            if (enabled(r)) want.put(r.pcId, r);
        }
        for (Iterator<Map.Entry<String, Worker>> it = workers.entrySet().iterator(); it.hasNext(); ) {
            Map.Entry<String, Worker> e = it.next();
            Worker w = e.getValue();
            PcRecord r = want.get(e.getKey());
            if (r == null || !r.bt.equals(w.bt)) {     /* 关了 / 删了 / 换了蓝牙地址 */
                Log.i(App.TAG, "[离开锁屏] 停止 " + w.title);
                w.finish();
                ending.add(w);
                it.remove();
            } else {
                w.setGrace(r.awayGrace);
            }
        }
        for (PcRecord r : want.values()) {
            if (workers.containsKey(r.pcId) || endingFor(r.pcId)) continue;   /* 旧的收完尾再开新的 */
            Log.i(App.TAG, "[离开锁屏] 开启 " + r.title() + "（" + r.awayGrace + " 秒）");
            Worker w = new Worker(r);
            workers.put(r.pcId, w);
            states.put(r.pcId, "正在启动…");
            w.start();
        }
        for (String id : new ArrayList<>(states.keySet())) {
            if (!workers.containsKey(id) && !endingFor(id)) states.remove(id);
        }
        changed();
        if (workers.isEmpty() && ending.isEmpty()) {
            instance = null;                         /* 之后再开就起一个新的 */
            stopForeground(true);
            stopSelf();
        }
    }

    private boolean endingFor(String pcId) {
        for (Worker w : ending) {
            if (w.pcId.equals(pcId)) return true;
        }
        return false;
    }

    /** 工作线程退出（主线程）。出错退出的过一会儿再按记录重开，免得一直崩一直开。 */
    private void exited(Worker w) {
        if (!ending.remove(w) && !workers.values().remove(w)) return;
        if (!w.crashed) {
            resync();
            return;
        }
        main.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (instance == AwayService.this) resync();
            }
        }, RETRY_MS);
    }

    /** 状态变了：刷新通知，告诉界面（主线程）。 */
    private void changed() {
        if (instance != this) return;
        if (!workers.isEmpty()) {
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) nm.notify(NOTE_ID, note());
        }
        Runnable l = listener;
        if (l != null) l.run();
    }

    private Notification note() {
        String title = "离开自动锁屏";
        StringBuilder text = new StringBuilder();
        for (Worker w : workers.values()) {
            if (workers.size() == 1) {
                title += " · " + w.title;
            } else {
                if (text.length() > 0) text.append('\n');
                text.append(w.title).append("：");
            }
            text.append(state(w.pcId)).append("（手机离开 ").append(Ui.duration(w.grace)).append("后锁屏）");
        }
        if (text.length() == 0) text.append("正在启动…");
        PendingIntent pi = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_note)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setContentIntent(pi)
                .setOngoing(true)
                .setShowWhen(false)
                .setOnlyAlertOnce(true)
                .build();
    }

    private BluetoothAdapter adapter() {
        BluetoothManager bm = getSystemService(BluetoothManager.class);
        return bm == null ? null : bm.getAdapter();
    }

    /** 没法用蓝牙的原因；能用返回 null。 */
    private String btProblem(BluetoothAdapter ad) {
        if (ad == null) return "手机没有蓝牙";
        if (Build.VERSION.SDK_INT >= 31
                && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            return "没有“附近的设备”权限，请打开 App 重新开启";
        }
        if (!ad.isEnabled()) return "手机蓝牙关着";
        return null;
    }

    /* ==== 每台电脑一个工作线程 ==== */

    private final class Worker extends Thread {
        final String pcId;
        final String bt;
        final String title;
        private volatile int grace;
        private volatile boolean dirty;
        private volatile boolean stopping;
        volatile boolean crashed;
        private final Object lk = new Object();
        /** 当前的连接、是否还在连（lk 保护）。 */
        private BleTransport cur;
        private boolean connecting;
        /** 手机蓝牙开关变了：pause() 不用等满（lk 保护；每轮开头清掉）。 */
        private boolean poked;

        Worker(PcRecord r) {
            super("pu-away-" + r.pcId.substring(0, Math.min(8, r.pcId.length())));
            pcId = r.pcId;
            bt = r.bt;
            title = r.title();
            grace = r.awayGrace;
        }

        void setGrace(int g) {
            if (g == grace) return;
            grace = g;
            dirty = true;
            wake(false);
        }

        /** 停：连着就先发 AWAY 0 再断开；正在连就放弃。 */
        void finish() {
            stopping = true;
            wake(true);
        }

        /** 手机蓝牙打开 / 正在关闭（主线程）。 */
        void btChanged(boolean on) {
            synchronized (lk) {
                poked = true;
                if (!on && cur != null) cur.abort();
                lk.notifyAll();
            }
        }

        private void wake(boolean abortConnect) {
            synchronized (lk) {
                if (cur != null) {
                    if (connecting && abortConnect) cur.abort();
                    else cur.kick();
                }
                lk.notifyAll();
            }
        }

        private void state(final String s) {
            if (stopping && !s.startsWith("正在关")) return;
            states.put(pcId, s);
            main.post(new Runnable() {
                @Override
                public void run() {
                    changed();
                }
            });
        }

        /** 等一会儿（finish / setGrace / 手机蓝牙开关会提前叫醒）。 */
        private void pause(long ms) throws InterruptedException {
            long end = System.currentTimeMillis() + ms;
            synchronized (lk) {
                while (!stopping && !dirty && !poked) {
                    long left = end - System.currentTimeMillis();
                    if (left <= 0) return;
                    lk.wait(left);
                }
            }
        }

        private void drop(BleTransport b) {
            synchronized (lk) {
                if (cur == b) cur = null;
                connecting = false;
            }
            b.close();
        }

        @Override
        public void run() {
            boolean direct = true;
            try {
                while (!stopping) {
                    PcRecord r = Store.get(AwayService.this, pcId);
                    if (r == null) break;
                    dirty = false;
                    synchronized (lk) {
                        poked = false;
                    }
                    BluetoothAdapter ad = adapter();
                    String why = btProblem(ad);
                    if (why != null) {
                        state(why + "，稍后再试");
                        pause(RETRY_MS);
                        direct = true;
                        continue;
                    }
                    BleTransport b = BleTransport.background(AwayService.this, ad, bt, !direct);
                    synchronized (lk) {
                        if (stopping) break;
                        cur = b;
                        connecting = true;
                    }
                    try {
                        state(direct ? "正在连接电脑蓝牙…" : "电脑不在附近，等它回来自动连上");
                        b.connectBy(direct ? System.currentTimeMillis() + DIRECT_MS : Long.MAX_VALUE);
                    } catch (IOException e) {
                        Log.i(App.TAG, "[离开锁屏] " + (direct ? "直连" : "自动连接") + " " + bt + " 没成：" + e.getMessage());
                        drop(b);
                        if (stopping) break;
                        if (direct) {
                            direct = false;
                        } else {
                            state("连不上电脑，稍后再试");
                            pause(RETRY_MS);
                            direct = true;
                        }
                        continue;
                    }
                    synchronized (lk) {
                        connecting = false;
                    }
                    try {
                        serve(b, r);
                    } finally {
                        drop(b);
                    }
                    direct = true;
                }
            } catch (InterruptedException e) {
                Log.i(App.TAG, "[离开锁屏] 线程被打断");
            } catch (RuntimeException e) {
                Log.e(App.TAG, "[离开锁屏] 出错", e);
                crashed = true;
            } finally {
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        exited(Worker.this);
                    }
                });
            }
        }

        /** 连着的时候：发 HELLO + AWAY，之后每 REFRESH_MS 再发；断开、要停或一直失败时返回。 */
        private void serve(BleTransport b, PcRecord r) throws InterruptedException {
            long next = 0;
            boolean low = false;
            int fails = 0;
            while (true) {
                if (stopping) {
                    sendOff(b, r);
                    return;
                }
                if (dirty) {
                    dirty = false;
                    next = 0;
                }
                long now = System.currentTimeMillis();
                if (now >= next) {
                    int g = grace;
                    try {
                        if (!exchange(b, r, g)) {
                            next = now + BUSY_RETRY_MS;          /* 解锁界面正用着 */
                        } else {
                            fails = 0;
                            next = now + REFRESH_MS;
                            state("已生效，连着电脑");
                            if (!low) {
                                b.lowPower();
                                low = true;
                            }
                        }
                    } catch (WrongPcException e) {
                        state("这个蓝牙地址不是配对的那台电脑");
                        next = now + PEER_RETRY_MS;
                    } catch (PeerException e) {
                        state(peerText(e));
                        next = now + (e.code == Proto.E_NO_SERVICE || e.code == Proto.E_BUSY ? RETRY_MS : PEER_RETRY_MS);
                    } catch (IOException e) {
                        if (b.isDisconnected()) {
                            state("蓝牙断开了");
                            return;
                        }
                        Log.i(App.TAG, "[离开锁屏] 请求失败：" + e);
                        if (++fails >= MAX_FAILS) {
                            state("电脑一直不回应，重新连接");
                            return;
                        }
                        next = now + RETRY_MS;
                    }
                }
                if (stopping || dirty) continue;
                if (b.waitDisconnect(Math.max(1, next - System.currentTimeMillis()))) {
                    Log.i(App.TAG, "[离开锁屏] 蓝牙断开了 " + bt);
                    state("蓝牙断开了");
                    return;
                }
            }
        }

        /** HELLO + AWAY g；解锁界面正用着蓝牙返回 false。 */
        private boolean exchange(BleTransport b, PcRecord r, int g) throws IOException {
            if (!LinkGate.bgEnter()) return false;
            try {
                b.drain();
                Client c = new Client(b);
                Client.Chal ch = c.hello(r.phoneId, r.s, READ_MS);
                c.away(ch, g, READ_MS);
                return true;
            } finally {
                LinkGate.bgLeave();
            }
        }

        /** 关掉：告诉电脑这次断开不算离开（解锁界面正用着就等它一会儿）。 */
        private void sendOff(BleTransport b, PcRecord r) {
            state("正在关闭…");
            long end = System.currentTimeMillis() + OFF_WAIT_MS;
            try {
                while (!b.isDisconnected()) {
                    if (exchange(b, r, 0)) {
                        Log.i(App.TAG, "[离开锁屏] 已告诉电脑关闭 " + title);
                        return;
                    }
                    if (System.currentTimeMillis() > end) break;
                    Thread.sleep(300);
                }
                Log.w(App.TAG, "[离开锁屏] 没能告诉电脑关闭（" + (b.isDisconnected() ? "已断开" : "解锁界面一直占着") + "）");
            } catch (IOException | InterruptedException e) {
                Log.w(App.TAG, "[离开锁屏] 发 AWAY 0 失败：" + e);
            }
        }
    }

    private static String peerText(PeerException e) {
        switch (e.code) {
            case Proto.E_NOT_PAIRED:
                return "电脑上没有这部手机的配对记录";
            case Proto.E_BAD_REQUEST:
                return "电脑那边不支持离开自动锁屏（请更新电脑上的程序）";
            case Proto.E_NO_SERVICE:
                return "电脑上的解锁服务没运行，稍后再试";
            default:
                return "电脑拒绝了：" + Ui.errorText(e);
        }
    }
}
