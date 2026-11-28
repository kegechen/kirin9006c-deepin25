package org.hisi.phoneunlock;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricPrompt;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.security.keystore.KeyPermanentlyInvalidatedException;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.hisi.phoneunlock.proto.Client;
import org.hisi.phoneunlock.proto.PeerException;
import org.hisi.phoneunlock.proto.Proto;
import org.hisi.phoneunlock.proto.Transport;
import org.hisi.phoneunlock.proto.WrongPcException;

import java.io.IOException;
import java.security.GeneralSecurityException;
import java.security.Signature;
import java.security.SignatureException;

/**
 * 解锁：连接 → HELLO（解 box 拿电脑信息）→ 可选 SHOT → 用户按“验证指纹解锁” →
 * BiometricPrompt + CryptoObject(SHA256withECDSA) 签 U → UNLOCK。
 * 测试用的一键模式（rec.oneTap，密钥不要求指纹）：按“一键解锁”直接签 U，不弹指纹。
 * “一键锁屏”：HELLO 之后发 LOCK（ks 算的 HMAC，不用指纹）。EXTRA_LOCK 打开时直接锁屏，不拿截图。
 * 用蓝牙连着的整个期间占着 LinkGate，后台的离开自动锁屏服务不插话。
 */
public class UnlockActivity extends WorkActivity {
    static final String EXTRA_PC = "pc_id";
    static final String EXTRA_LOCK = "lock";

    /** 挑战 120 秒有效、电脑空闲 90 秒断开：超过这个年龄就先重新 HELLO 再弹指纹。 */
    private static final long CHAL_MAX_AGE_MS = 80_000;

    private enum Stage { CONNECTING, READY, NO_SESSION, BUSY, DONE, FAILED }

    private PcRecord rec;

    /* 界面（主线程） */
    private TextView title;
    private TextView userLine;
    private TextView lockLine;
    private TextView connLine;
    private ImageView shot;
    private TextView shotHint;
    private ProgressBar progress;
    private TextView statusView;
    private Button mainBtn;
    private Button refreshBtn;
    private Button lockBtn;
    private Stage stage = Stage.CONNECTING;
    private Client.Chal uiChal;
    private CancellationSignal cancel;
    private boolean haveShot;

    /* 会话状态（只在后台线程里改） */
    private volatile Transport transport;
    private Client client;
    private Client.Chal chal;
    private volatile boolean retried4;
    /** 是否占着 LinkGate（gateLock 保护）。 */
    private final Object gateLock = new Object();
    private boolean gateHeld;

    /** 当前的解锁界面（主线程）。同一时间只留一个：两个界面在同一条蓝牙链路上会互相吃掉对方的回复。 */
    private static UnlockActivity current;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (current != null && current != this && !current.isFinishing()) current.finish();
        current = this;
        rec = Store.get(this, getIntent().getStringExtra(EXTRA_PC));
        if (rec == null) {
            Toast.makeText(this, "找不到这台电脑的配对记录", Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        buildUi();
        if (getIntent().getBooleanExtra(EXTRA_LOCK, false)) startLock();
        else refreshAll(true);
    }

    private void buildUi() {
        int pad = Ui.dp(this, 20);
        ScrollView sv = new ScrollView(this);
        LinearLayout col = Ui.vbox(this);
        col.setPadding(pad, pad, pad, pad);
        sv.addView(col, new ViewGroup.LayoutParams(-1, -2));

        title = Ui.text(this, rec.title(), 24, Ui.TEXT, true);
        col.addView(title);
        userLine = Ui.text(this, "用户：" + rec.user, 16, Ui.TEXT, false);
        col.addView(userLine, Ui.lpTop(this, -1, -2, 8));
        lockLine = Ui.text(this, "状态：—", 16, Ui.TEXT, false);
        col.addView(lockLine, Ui.lpTop(this, -1, -2, 4));
        connLine = Ui.text(this, "连接：—", 14, Ui.TEXT2, false);
        col.addView(connLine, Ui.lpTop(this, -1, -2, 4));
        if (rec.oneTap) {
            String warn = "测试模式：一键解锁，不验证指纹";
            if (KeyHelper.notReadyReason(this) == null) {
                warn += "\n手机已经能用指纹了，建议删掉这台电脑重新配对，改成指纹解锁。";
            }
            TextView w = Ui.text(this, warn, 14, Ui.ERR_RED, false);
            w.setLineSpacing(0, 1.2f);
            col.addView(w, Ui.lpTop(this, -1, -2, 6));
        }

        FrameLayout shotBox = new FrameLayout(this);
        shotBox.setBackground(Ui.round(0xFF263238, Ui.dp(this, 10), 0, 0));
        shotBox.setMinimumHeight(Ui.dp(this, 160));
        shot = new ImageView(this);
        shot.setAdjustViewBounds(true);
        shot.setScaleType(ImageView.ScaleType.FIT_CENTER);
        shot.setMaxHeight(Ui.dp(this, 320));
        shot.setVisibility(View.GONE);
        shotBox.addView(shot, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER));
        shotHint = Ui.text(this, "锁屏截图", 14, 0xFFB0BEC5, false);
        shotHint.setGravity(Gravity.CENTER);
        int sp = Ui.dp(this, 12);
        shotHint.setPadding(sp, sp, sp, sp);
        shotBox.addView(shotHint, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER));
        shotBox.setClipToOutline(true);
        col.addView(shotBox, Ui.lpTop(this, -1, -2, 16));

        LinearLayout st = Ui.hbox(this);
        progress = new ProgressBar(this);
        progress.setIndeterminate(true);
        st.addView(progress, new LinearLayout.LayoutParams(Ui.dp(this, 28), Ui.dp(this, 28)));
        statusView = Ui.text(this, "", 16, Ui.TEXT, false);
        statusView.setLineSpacing(0, 1.2f);
        LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(0, -2, 1);
        sl.leftMargin = Ui.dp(this, 10);
        st.addView(statusView, sl);
        col.addView(st, Ui.lpTop(this, -1, -2, 16));

        mainBtn = Ui.bigButton(this, authLabel());
        mainBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onMainButton();
            }
        });
        col.addView(mainBtn, Ui.lpTop(this, -1, Ui.dp(this, 64), 20));

        refreshBtn = Ui.lightButton(this, "刷新");
        refreshBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                refreshAll(true);
            }
        });
        col.addView(refreshBtn, Ui.lpTop(this, -1, Ui.dp(this, 46), 12));

        lockBtn = Ui.lightButton(this, "一键锁屏");
        lockBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startLock();
            }
        });
        col.addView(lockBtn, Ui.lpTop(this, -1, Ui.dp(this, 46), 12));

        setContentView(sv);
        apply(Stage.CONNECTING, "正在连接…");
    }

    /* ==== 界面状态 ==== */

    private String authLabel() {
        return rec.oneTap ? "一键解锁（测试）" : "验证指纹解锁";
    }

    private void apply(Stage s, String msg) {
        stage = s;
        progress.setVisibility(s == Stage.CONNECTING || s == Stage.BUSY ? View.VISIBLE : View.GONE);
        statusView.setText(msg);
        statusView.setTextColor(s == Stage.FAILED ? Ui.ERR_RED : s == Stage.DONE ? Ui.OK_GREEN : Ui.TEXT);
        statusView.setTextSize(s == Stage.DONE ? 22 : 16);
        switch (s) {
            case READY:
                mainBtn.setText(authLabel());
                Ui.setBigEnabled(this, mainBtn, true);
                break;
            case DONE:
                mainBtn.setText("完成");
                Ui.setBigEnabled(this, mainBtn, true);
                break;
            case FAILED:
                mainBtn.setText("重试");
                Ui.setBigEnabled(this, mainBtn, true);
                break;
            default:
                mainBtn.setText(authLabel());
                Ui.setBigEnabled(this, mainBtn, false);
        }
        refreshBtn.setVisibility(s == Stage.READY || s == Stage.NO_SESSION || s == Stage.DONE ? View.VISIBLE : View.GONE);
        lockBtn.setVisibility(s == Stage.READY || s == Stage.DONE ? View.VISIBLE : View.GONE);
    }

    /** 后台线程调用：连同当前的 chal 一起交给界面。 */
    private void stage(final Stage s, final String msg) {
        final Client.Chal c = chal;
        runUi(new Runnable() {
            @Override
            public void run() {
                uiChal = c;
                apply(s, msg);
            }
        });
    }

    @Override
    public void status(final String text) {
        runUi(new Runnable() {
            @Override
            public void run() {
                statusView.setText(text);
            }
        });
    }

    private void onMainButton() {
        switch (stage) {
            case READY:
                retried4 = false;
                Client.Chal c = uiChal;
                if (c == null || c.used || System.currentTimeMillis() - c.at > CHAL_MAX_AGE_MS) {
                    apply(Stage.BUSY, "正在重新获取挑战…");
                    runBg(new Runnable() {
                        @Override
                        public void run() {
                            opHello(false, true);
                        }
                    });
                } else {
                    startAuth();
                }
                break;
            case FAILED:
                refreshAll(!haveShot);
                break;
            case DONE:
                finish();
                break;
            default:
                break;
        }
    }

    private void refreshAll(final boolean wantShot) {
        apply(Stage.CONNECTING, "正在连接…");
        runBg(new Runnable() {
            @Override
            public void run() {
                opHello(wantShot, false);
            }
        });
    }

    /* ==== 后台：连接 / HELLO / SHOT / UNLOCK ==== */

    private long readTimeout() {
        Transport t = transport;
        return t != null && t.isBluetooth() ? 15000 : 10000;
    }

    /** 占 LinkGate：后台服务正在收发就等它发完。界面关掉了抛 InterruptedException。 */
    private void acquireGate() throws InterruptedException {
        synchronized (gateLock) {
            if (gateHeld) return;
        }
        LinkGate.uiEnter();
        synchronized (gateLock) {
            if (destroyed || gateHeld) {
                LinkGate.uiLeave();
                if (destroyed) throw new InterruptedException("界面已关闭");
                return;
            }
            gateHeld = true;
        }
    }

    private void releaseGate() {
        synchronized (gateLock) {
            if (!gateHeld) return;
            gateHeld = false;
            LinkGate.uiLeave();
        }
    }

    private void ensureConnected() throws IOException, InterruptedException {
        if (transport != null) return;
        status("正在连接…");
        acquireGate();
        Transport t;
        try {
            t = new Connector(this, this).connect(rec.target(), Store.mode(this));
        } catch (IOException | InterruptedException | RuntimeException e) {
            releaseGate();
            throw e;
        }
        /* 只有蓝牙和后台服务共用链路；Wi-Fi 是另一条会话，电脑那边每台手机能同时留两个挑战，不冲突 */
        if (!t.isBluetooth()) releaseGate();
        transport = t;
        client = new Client(t);
        final String via = t.describe();
        runUi(new Runnable() {
            @Override
            public void run() {
                connLine.setText("连接：" + via);
            }
        });
    }

    private void dropTransport() {
        Transport t = transport;
        transport = null;
        client = null;
        chal = null;
        closeAsync(t);
        releaseGate();
    }

    /** HELLO（没连就先连；旧连接不通就重连一次）。 */
    private Client.Chal helloRetry() throws IOException, InterruptedException {
        boolean fresh = transport == null;
        ensureConnected();
        status("正在确认电脑身份…");
        try {
            transport.drain();
            return client.hello(rec.phoneId, rec.s, readTimeout());
        } catch (IOException e) {
            if (fresh || e instanceof PeerException || e instanceof WrongPcException) throw e;
            /* 旧连接可能已被电脑断开（空闲 90 秒） */
            Log.i(App.TAG, "旧连接不可用，重连：" + e);
            dropTransport();
            ensureConnected();
            return client.hello(rec.phoneId, rec.s, readTimeout());
        }
    }

    /** HELLO + 显示电脑信息。thenPrompt：成功后自动弹指纹。 */
    private void opHello(boolean wantShot, boolean thenPrompt) {
        try {
            Client.Chal c = helloRetry();
            chal = c;
            showInfo(c);
            if (wantShot && !fetchShot(c)) {
                /* 截图那步把连接弄断了：重连再 HELLO 一次（不再要截图） */
                ensureConnected();
                c = client.hello(rec.phoneId, rec.s, readTimeout());
                chal = c;
                showInfo(c);
            }
            if ("0".equals(c.get("session"))) {
                stage(Stage.NO_SESSION, "该用户没有登录图形会话，没法解锁。\n请先在电脑上登录桌面，再点“刷新”。");
                return;
            }
            String ready = rec.oneTap
                    ? (thenPrompt ? "正在解锁…" : "已连上，按下面的按钮一键解锁")
                    : (thenPrompt ? "请验证指纹" : "已连上，按下面的按钮验证指纹解锁");
            stage(Stage.READY, ready);
            if (thenPrompt) {
                runUi(new Runnable() {
                    @Override
                    public void run() {
                        startAuth();
                    }
                });
            }
        } catch (WrongPcException e) {
            dropTransport();
            stage(Stage.FAILED, Ui.errorText(e));
        } catch (PeerException e) {
            peerFailed(e);
        } catch (IOException e) {
            dropTransport();
            stage(Stage.FAILED, "连不上电脑：\n" + Ui.errorText(e));
        } catch (InterruptedException e) {
            dropTransport();
        }
    }

    /** 拿截图；返回 false 表示连接坏了（已丢弃，需要重连）。 */
    private boolean fetchShot(Client.Chal c) {
        if (!"1".equals(c.get("shot"))) {
            final String why = "1".equals(c.get("locked")) ? "电脑没有提供截图" : "电脑没有锁屏，不显示截图";
            runUi(new Runnable() {
                @Override
                public void run() {
                    showShotText(why);
                }
            });
            return true;
        }
        status("正在获取锁屏截图…");
        Transport t = transport;
        int width = t.isBluetooth() ? 432 : 720;
        try {
            byte[] jpg = client.shot(c, width, t.isBluetooth() ? 25000 : 15000);
            final Bitmap bm = BitmapFactory.decodeByteArray(jpg, 0, jpg.length);
            Log.i(App.TAG, "截图 " + jpg.length + " 字节" + (bm == null ? "（解码失败）" : " " + bm.getWidth() + "x" + bm.getHeight()));
            runUi(new Runnable() {
                @Override
                public void run() {
                    if (bm != null) {
                        haveShot = true;
                        shot.setImageBitmap(bm);
                        shot.setVisibility(View.VISIBLE);
                        shotHint.setVisibility(View.GONE);
                    } else {
                        showShotText("截图格式不对，显示不了");
                    }
                }
            });
            return true;
        } catch (final PeerException e) {
            runUi(new Runnable() {
                @Override
                public void run() {
                    showShotText("没有截图：" + Ui.errorText(e));
                }
            });
            return true;
        } catch (final IOException e) {
            Log.i(App.TAG, "截图失败：" + e);
            dropTransport();
            runUi(new Runnable() {
                @Override
                public void run() {
                    showShotText("截图没拿到（" + Ui.errorText(e) + "）");
                }
            });
            return false;
        }
    }

    private void showShotText(String s) {
        shot.setVisibility(View.GONE);
        shotHint.setVisibility(View.VISIBLE);
        shotHint.setText(s);
    }

    private void showInfo(final Client.Chal c) {
        final String host = c.get("host").isEmpty() ? rec.title() : c.get("host");
        String user = c.get("user").isEmpty() ? rec.user : c.get("user");
        String real = c.get("realname");
        final String userText = "用户：" + user + (real.isEmpty() ? "" : "（" + real + "）");
        final String lockText;
        if ("0".equals(c.get("session"))) {
            lockText = "状态：没有登录图形会话";
        } else if ("1".equals(c.get("locked"))) {
            String sec = c.get("locked_for");
            String dur = "";
            try {
                if (!sec.isEmpty()) dur = "，已锁 " + Ui.duration(Long.parseLong(sec));
            } catch (NumberFormatException ignored) {
            }
            lockText = "状态：已锁屏" + dur;
        } else {
            lockText = "状态：没有锁屏（或无法确定）";
        }
        Log.i(App.TAG, "电脑信息：session=" + c.get("session") + " locked=" + c.get("locked") + " shot=" + c.get("shot"));
        runUi(new Runnable() {
            @Override
            public void run() {
                title.setText(host);
                userLine.setText(userText);
                lockLine.setText(lockText);
            }
        });
    }

    private void peerFailed(PeerException e) {
        final String msg = Ui.errorText(e);
        if (e.code == Proto.E_NOT_PAIRED) {
            stage(Stage.FAILED, msg);
            runUi(new Runnable() {
                @Override
                public void run() {
                    repairDialog("电脑上已经没有这部手机的配对记录（可能在电脑上被删除了）。要重新配对吗？");
                }
            });
        } else {
            stage(Stage.FAILED, msg);
        }
    }

    /** 指纹通过之后：签 U，发 UNLOCK。 */
    private void opUnlock(Client.Chal c, Signature sig) {
        try {
            if (c != chal || transport == null) {
                stage(Stage.READY, "连接已经换过了，请再按一次");
                return;
            }
            sig.update(Proto.unlockU(rec.pcId, rec.phoneId, c.cn, c.pn));
            byte[] der = sig.sign();
            c.used = true;
            client.unlock(der, readTimeout());
            Log.i(App.TAG, "解锁成功");
            runUi(new Runnable() {
                @Override
                public void run() {
                    lockLine.setText("状态：已解锁");
                }
            });
            stage(Stage.DONE, "已解锁");
        } catch (SignatureException e) {
            Log.w(App.TAG, "签名失败", e);
            stage(Stage.READY, "签名失败：" + e.getMessage());
        } catch (PeerException e) {
            if (e.code == Proto.E_STALE_CHAL && !retried4) {
                retried4 = true;
                status("挑战已过期，正在重新获取…");
                opHello(false, true);
                return;
            }
            if (e.code == Proto.E_NOT_PAIRED) {
                peerFailed(e);
                return;
            }
            stage(Stage.READY, Ui.errorText(e) + "\n可以再按一次重试。");
        } catch (IOException e) {
            dropTransport();
            stage(Stage.FAILED, "连接断了：" + Ui.errorText(e) + "\n点“重试”重新连接。");
        }
    }

    private void startLock() {
        if (destroyed) return;
        apply(Stage.BUSY, "正在锁屏…");
        runBg(new Runnable() {
            @Override
            public void run() {
                opLock();
            }
        });
    }

    /** 一键锁屏：手上的挑战还能用就直接 LOCK，否则先 HELLO。不用指纹。 */
    private void opLock() {
        try {
            Client.Chal c = chal;
            if (transport == null || c == null || c.used || System.currentTimeMillis() - c.at > CHAL_MAX_AGE_MS) {
                c = helloRetry();
                chal = c;
                showInfo(c);
            }
            if ("0".equals(c.get("session"))) {
                stage(Stage.NO_SESSION, "该用户没有登录图形会话，不用锁屏。");
                return;
            }
            status("正在锁屏…");
            try {
                client.lock(c, readTimeout());
            } catch (PeerException e) {
                if (e.code != Proto.E_STALE_CHAL) throw e;
                status("挑战已过期，正在重新获取…");
                c = helloRetry();
                chal = c;
                showInfo(c);
                client.lock(c, readTimeout());
            }
            Log.i(App.TAG, "锁屏成功");
            runUi(new Runnable() {
                @Override
                public void run() {
                    lockLine.setText("状态：已锁屏");
                }
            });
            stage(Stage.DONE, "已锁屏");
        } catch (WrongPcException e) {
            dropTransport();
            stage(Stage.FAILED, Ui.errorText(e));
        } catch (PeerException e) {
            peerFailed(e);
        } catch (IOException e) {
            dropTransport();
            stage(Stage.FAILED, "锁屏没成功：\n" + Ui.errorText(e));
        } catch (InterruptedException e) {
            dropTransport();
        }
    }

    /* ==== 指纹 / 一键 ==== */

    private void startAuth() {
        if (rec.oneTap) startOneTap();
        else startBiometric();
    }

    /** 拿私钥初始化签名器；失败时已经更新界面（必要时弹重新配对），返回 null。 */
    private Signature signer() {
        try {
            return KeyHelper.initSignature(rec.alias());
        } catch (KeyPermanentlyInvalidatedException e) {
            Log.i(App.TAG, "密钥已失效（指纹有变动）");
            apply(Stage.READY, "手机指纹有变动，需要重新配对");
            repairDialog("手机指纹有变动（新录入或删除了指纹），原来的密钥已经失效，需要重新配对。");
        } catch (KeyHelper.MissingKeyException e) {
            apply(Stage.READY, "手机里找不到这台电脑的密钥，需要重新配对");
            repairDialog("手机里找不到这台电脑的密钥，需要重新配对。");
        } catch (GeneralSecurityException e) {
            Log.w(App.TAG, "初始化签名失败", e);
            apply(Stage.READY, "没法使用密钥：" + e.getMessage());
        }
        return null;
    }

    /** 测试用的一键模式：密钥不要求指纹，直接签名发 UNLOCK。 */
    private void startOneTap() {
        if (destroyed) return;
        final Client.Chal c = uiChal;
        if (c == null || c.used) {
            apply(Stage.READY, "请再按一次");
            return;
        }
        final Signature sig = signer();
        if (sig == null) return;
        apply(Stage.BUSY, "正在解锁…");
        Log.i(App.TAG, "一键解锁（测试模式，不验证指纹）");
        runBg(new Runnable() {
            @Override
            public void run() {
                opUnlock(c, sig);
            }
        });
    }

    private void startBiometric() {
        if (destroyed) return;
        final Client.Chal c = uiChal;
        if (c == null || c.used) {
            apply(Stage.READY, "请再按一次");
            return;
        }
        Signature sig = signer();
        if (sig == null) return;

        String user = rec.user + (rec.realname.isEmpty() ? "" : "（" + rec.realname + "）");
        BiometricPrompt.Builder b = new BiometricPrompt.Builder(this)
                .setTitle("验证指纹解锁电脑")
                .setSubtitle(title.getText())
                .setDescription(userLine.getText().length() > 0 ? userLine.getText() : "用户：" + user);
        if (Build.VERSION.SDK_INT >= 30) {
            b.setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG);
        }
        /* 只允许强生物识别时必须有取消按钮（28/29 也一样要） */
        b.setNegativeButton("取消", getMainExecutor(), new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface d, int which) {
                if (!destroyed) apply(Stage.READY, "已取消");
            }
        });
        BiometricPrompt prompt = b.build();
        cancel = new CancellationSignal();
        apply(Stage.BUSY, "请按指纹…");
        Log.i(App.TAG, "弹出指纹验证");
        try {
            prompt.authenticate(new BiometricPrompt.CryptoObject(sig), cancel, getMainExecutor(),
                    new BiometricPrompt.AuthenticationCallback() {
                        @Override
                        public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult r) {
                            if (destroyed) return;
                            Log.i(App.TAG, "指纹验证通过");
                            final Signature s = r.getCryptoObject().getSignature();
                            apply(Stage.BUSY, "正在解锁…");
                            runBg(new Runnable() {
                                @Override
                                public void run() {
                                    opUnlock(c, s);
                                }
                            });
                        }

                        @Override
                        public void onAuthenticationError(int code, CharSequence msg) {
                            if (destroyed) return;
                            Log.i(App.TAG, "指纹验证结束 code=" + code);
                            if (code == BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED
                                    || code == BiometricPrompt.BIOMETRIC_ERROR_CANCELED) {
                                apply(Stage.READY, "已取消");
                            } else {
                                apply(Stage.READY, "指纹验证没完成：" + msg);
                            }
                        }

                        @Override
                        public void onAuthenticationFailed() {
                            /* 没认出来：系统框里会提示再试，不用处理 */
                        }
                    });
        } catch (RuntimeException e) {
            Log.w(App.TAG, "弹指纹失败", e);
            apply(Stage.READY, "没法弹出指纹验证：" + e.getMessage());
        }
    }

    private void repairDialog(String msg) {
        if (destroyed || isFinishing()) return;
        new AlertDialog.Builder(this)
                .setTitle("需要重新配对")
                .setMessage(msg + "\n\n请先在电脑上运行 " + Ui.PAIR_CMD + "，再扫码。")
                .setPositiveButton("重新配对（扫码）", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        startActivity(new Intent(UnlockActivity.this, ScanActivity.class));
                        finish();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    @Override
    protected void onDestroy() {
        if (current == this) current = null;
        if (cancel != null) cancel.cancel();
        Transport t = transport;
        transport = null;
        closeAsync(t);
        super.onDestroy();                          /* 置 destroyed，之后 acquireGate 不会再占 */
        releaseGate();
    }
}
