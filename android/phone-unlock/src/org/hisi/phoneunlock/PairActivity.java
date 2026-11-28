package org.hisi.phoneunlock;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.InputType;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.hisi.phoneunlock.proto.Client;
import org.hisi.phoneunlock.proto.HostPort;
import org.hisi.phoneunlock.proto.PairUri;
import org.hisi.phoneunlock.proto.PeerException;
import org.hisi.phoneunlock.proto.Proto;
import org.hisi.phoneunlock.proto.Target;
import org.hisi.phoneunlock.proto.Transport;

import java.io.IOException;
import java.security.GeneralSecurityException;
import java.security.PublicKey;
import java.util.Arrays;
import java.util.List;

/** 配对：解析 hisiphone://pair?...（扫码 / VIEW intent / 粘贴）→ 确认 → 生成密钥 → PAIR → 核对 mac2 → 保存 S。 */
public class PairActivity extends WorkActivity {
    private TextView detail;
    private TextView statusView;
    private ProgressBar progress;
    private Button action;
    private PairUri pu;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        int pad = Ui.dp(this, 20);
        ScrollView sv = new ScrollView(this);
        LinearLayout col = Ui.vbox(this);
        col.setPadding(pad, pad, pad, pad);
        sv.addView(col, new ViewGroup.LayoutParams(-1, -2));

        col.addView(Ui.text(this, "配对电脑", 22, Ui.TEXT, true));
        detail = Ui.text(this, "", 15, Ui.TEXT, false);
        detail.setLineSpacing(0, 1.25f);
        col.addView(detail, Ui.lpTop(this, -1, -2, 12));
        progress = new ProgressBar(this);
        progress.setIndeterminate(true);
        progress.setVisibility(View.GONE);
        col.addView(progress, Ui.lpTop(this, -2, -2, 20));
        statusView = Ui.text(this, "", 16, Ui.TEXT, false);
        statusView.setLineSpacing(0, 1.2f);
        col.addView(statusView, Ui.lpTop(this, -1, -2, 12));
        action = Ui.bigButton(this, "完成");
        action.setVisibility(View.GONE);
        col.addView(action, Ui.lpTop(this, -1, -2, 24));
        setContentView(sv);

        Uri data = getIntent().getData();
        String link = data != null ? data.toString() : getIntent().getStringExtra("uri");
        try {
            pu = PairUri.parse(link);
        } catch (IllegalArgumentException e) {
            Log.i(App.TAG, "配对链接无效：" + e.getMessage());
            showDone(false, "配对链接无效：" + e.getMessage());
            return;
        }
        Log.i(App.TAG, "配对链接：pc_id=" + pu.pcId + " ip=" + pu.ips + " bt=" + pu.bt);
        detail.setText(describe(pu));
        confirm();
    }

    private static String describe(PairUri p) {
        StringBuilder sb = new StringBuilder();
        sb.append("电脑：").append(p.host.isEmpty() ? "（未知）" : p.host).append('\n');
        sb.append("用户：").append(p.user.isEmpty() ? "（未知）" : p.user).append('\n');
        sb.append("IP：").append(p.ips.isEmpty() ? "（无）" : join(p.ips)).append("　端口 ").append(p.port).append('\n');
        sb.append("蓝牙：").append(p.bt.isEmpty() ? "（无）" : p.bt).append('\n');
        sb.append("电脑 ID：").append(p.pcId);
        return sb.toString();
    }

    private static String join(List<String> l) {
        StringBuilder sb = new StringBuilder();
        for (String s : l) {
            if (sb.length() > 0) sb.append(", ");
            sb.append(s);
        }
        return sb.toString();
    }

    private void confirm() { confirm(null, null); }

    /** presetName / presetExtra：测试连接后重建对话框时把用户填过的东西带回来，别悄悄恢复默认值。 */
    private void confirm(String presetName, String presetExtra) {
        final PcRecord old = Store.get(this, pu.pcId);
        LinearLayout box = Ui.vbox(this);
        int p = Ui.dp(this, 22);
        box.setPadding(p, Ui.dp(this, 8), p, 0);
        String msg = "电脑：" + (pu.host.isEmpty() ? "（未知）" : pu.host) + "\n用户：" + (pu.user.isEmpty() ? "（未知）" : pu.user);
        if (old != null) msg += "\n\n这台电脑之前配对过，重新配对会替换原来的配对。";
        box.addView(Ui.text(this, msg, 16, Ui.TEXT, false));
        box.addView(Ui.text(this, "手机名称（锁屏和电脑上显示）", 13, Ui.TEXT2, false), Ui.lpTop(this, -1, -2, 16));
        final EditText name = new EditText(this);
        name.setSingleLine(true);
        name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        name.setHint(defaultName(false));
        box.addView(name, Ui.lp(-1, -2));
        name.setText(presetName != null ? presetName : defaultName(false));
        box.addView(Ui.text(this, "额外地址（可不填，逗号分隔的 host[:port]）", 13, Ui.TEXT2, false), Ui.lpTop(this, -1, -2, 16));
        final EditText extra = new EditText(this);
        extra.setSingleLine(true);
        extra.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        extra.setHint("例如 127.0.0.1");
        extra.setText(presetExtra != null ? presetExtra : (old != null ? old.extra : ""));
        box.addView(extra, Ui.lp(-1, -2));

        new AlertDialog.Builder(this)
                .setTitle("和这台电脑配对？")
                .setView(box)
                .setCancelable(false)
                .setPositiveButton("配对", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        final String ex = extra.getText().toString().trim();
                        final String nm = name.getText().toString().trim();
                        if (nm.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 64) {
                            showDone(false, "手机名称太长（最多 64 字节）");
                            return;
                        }
                        try {
                            HostPort.parseList(ex, pu.port);
                        } catch (IllegalArgumentException e) {
                            showDone(false, "额外地址格式不对：" + e.getMessage());
                            return;
                        }
                        String why = KeyHelper.notReadyReason(PairActivity.this);
                        if (why != null) {
                            notReady(why, ex, nm);
                            return;
                        }
                        start(ex, nm, false);
                    }
                })
                .setNeutralButton("测试连接", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        final String ex = extra.getText().toString().trim();
                        final String nm = name.getText().toString().trim();
                        try {
                            HostPort.parseList(ex, pu.port);
                        } catch (IllegalArgumentException e) {
                            showDone(false, "额外地址格式不对：" + e.getMessage());
                            return;
                        }
                        probe(ex, nm);
                    }
                })
                .setNegativeButton("取消", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        wipeCode();
                        finish();
                    }
                })
                .show();
    }

    /** 只连接 + PING（核对电脑 ID），Wi-Fi 和蓝牙各试一次；不生成密钥、不用配对码。测完回到确认框。 */
    private void probe(final String extra, final String presetName) {
        progress.setVisibility(View.VISIBLE);
        action.setVisibility(View.GONE);
        statusView.setTextColor(Ui.TEXT);
        statusView.setText("正在测试连接…");
        runBg(new Runnable() {
            @Override
            public void run() {
                Target target = new Target(pu.pcId, pu.ips, pu.port, HostPort.parseList(extra, pu.port), pu.bt);
                final StringBuilder sb = new StringBuilder();
                boolean any = false;
                for (String mode : new String[] {Store.MODE_WIFI, Store.MODE_BT}) {
                    String label = Store.MODE_WIFI.equals(mode) ? "Wi-Fi" : "蓝牙";
                    Transport tr = null;
                    long t0 = System.currentTimeMillis();
                    try {
                        tr = new Connector(PairActivity.this, PairActivity.this).connect(target, mode);
                        sb.append(label).append("：能连上（").append(tr.describe()).append("，")
                                .append(System.currentTimeMillis() - t0).append(" ms）\n");
                        any = true;
                    } catch (IOException e) {
                        sb.append(label).append("：连不上——").append(Ui.errorText(e)).append('\n');
                    } catch (InterruptedException e) {
                        return;
                    } finally {
                        closeAsync(tr);
                    }
                }
                Log.i(App.TAG, "测试连接：" + sb);
                final boolean ok = any;
                runUi(new Runnable() {
                    @Override
                    public void run() {
                        progress.setVisibility(View.GONE);
                        statusView.setTextColor(ok ? Ui.OK_GREEN : Ui.ERR_RED);
                        statusView.setText(sb.toString().trim());
                        if (!isFinishing()) confirm(presetName, extra);
                    }
                });
            }
        });
    }

    /** 不能用指纹：去设置，或者先用测试用的一键解锁模式配对。 */
    private void notReady(String why, final String extra, final String name) {
        showDone(false, why);
        new AlertDialog.Builder(this)
                .setTitle("现在还不能用指纹")
                .setMessage(why + "\n\n也可以先用“一键解锁”测试模式配对：解锁电脑时不验证指纹，点一下按钮就解锁。"
                        + "拿到这部手机的人都能解锁电脑，只建议测试时用。设置好指纹后删掉这台电脑重新配对，就是指纹解锁了。")
                .setNeutralButton("一键模式（测试）", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        start(extra, name, true);
                    }
                })
                .setPositiveButton("去设置", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        try {
                            startActivity(new Intent(Settings.ACTION_SECURITY_SETTINGS));
                        } catch (RuntimeException e) {
                            startActivity(new Intent(Settings.ACTION_SETTINGS));
                        }
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    private void start(final String extra, final String name, final boolean oneTap) {
        progress.setVisibility(View.VISIBLE);
        action.setVisibility(View.GONE);
        statusView.setTextColor(Ui.TEXT);
        statusView.setText("正在准备…");
        runBg(new Runnable() {
            @Override
            public void run() {
                doPair(extra, name, oneTap);
            }
        });
    }

    /** 默认手机名：自动读设备名称，没有再读设备型号（营销名），都读不到才用认证型号，见 DeviceName。 */
    private String defaultName(boolean oneTap) {
        return DeviceName.get(this) + (oneTap ? "（一键测试）" : "");
    }

    /** 后台线程。oneTap：测试用的一键解锁（密钥不要求指纹）。 */
    private void doPair(String extra, String chosenName, boolean oneTap) {
        final PcRecord old = Store.get(this, pu.pcId);
        String alias = PcRecord.aliasFor(pu.pcId);
        Transport tr = null;
        boolean ok = false;
        try {
            status("正在生成密钥…");
            String phoneId = Proto.newPhoneId();
            PublicKey pub = KeyHelper.generate(alias, oneTap);
            byte[] pubDer = pub.getEncoded();
            /* 一键模式在手机名后面做个记号，电脑上 list 时能看出这条配对不验证指纹 */
            String nm = chosenName == null || chosenName.trim().isEmpty() ? defaultName(oneTap) : chosenName.trim();
            if (oneTap && !nm.endsWith("（一键测试）")) {
                /* 先按 64 字节截断基础名再拼后缀：一键测试靠名字后缀标记，不能让它被 Proto.phoneName 截掉 */
                byte[] base = Proto.phoneName(nm);
                String limit = new String(base, java.nio.charset.StandardCharsets.UTF_8);
                nm = limit + "（一键测试）";
            }
            byte[] name = Proto.phoneName(nm);
            byte[] t = Proto.pairT(pu.pcId, phoneId, name, pubDer);
            byte[] mac = Proto.pairMac(pu.code, t);

            Target target = new Target(pu.pcId, pu.ips, pu.port, HostPort.parseList(extra, pu.port), pu.bt);
            tr = new Connector(this, this).connect(target, Store.mode(this));
            status("已连接（" + tr.describe() + "），正在配对…");
            long timeout = tr.isBluetooth() ? 15000 : 10000;
            byte[] mac2 = new Client(tr).pair(phoneId, name, pubDer, mac, timeout);
            if (!Proto.macEquals(mac2, Proto.pairMac2(pu.code, t))) {
                throw new IOException("电脑回复的校验值不对，配对失败（可能连到了别的电脑）");
            }

            PcRecord r = new PcRecord();
            r.pcId = pu.pcId;
            r.phoneId = phoneId;
            r.s = Proto.secret(pu.code, pu.pcId, phoneId);
            r.host = pu.host;
            r.user = pu.user;
            r.ips.addAll(pu.ips);
            r.port = pu.port;
            r.bt = pu.bt;
            r.extra = extra;
            r.pairedAt = System.currentTimeMillis();
            r.oneTap = oneTap;
            Store.put(this, r);
            ok = true;
            Log.i(App.TAG, "配对成功 pc_id=" + pu.pcId + "（" + tr.describe() + "）" + (oneTap ? " 一键测试模式" : ""));
            final String via = tr.describe();
            final String how = oneTap
                    ? "以后在主界面点这台电脑，再点“一键解锁”就能解锁（测试模式，不验证指纹）。"
                    : "以后在主界面点这台电脑，按指纹就能解锁。";
            runUi(new Runnable() {
                @Override
                public void run() {
                    showDone(true, "配对成功！\n" + how + "\n（本次经 " + via + "）");
                }
            });
        } catch (final PeerException e) {
            fail(Ui.errorText(e));
        } catch (GeneralSecurityException e) {
            Log.w(App.TAG, "生成密钥失败", e);
            fail("生成密钥失败：" + e.getMessage());
        } catch (IllegalStateException e) {
            /* 例如没有录指纹时 Keystore 不让生成需要认证的密钥 */
            Log.w(App.TAG, "生成密钥失败", e);
            fail("生成密钥失败：" + e.getMessage());
        } catch (IOException e) {
            fail("配对失败：" + Ui.errorText(e));
        } catch (InterruptedException e) {
            Log.i(App.TAG, "配对已取消");
        } finally {
            wipeCode();
            if (!ok) {
                /* 旧密钥在生成新密钥前已经删了，旧记录也就不能用了 */
                KeyHelper.delete(alias);
                if (old != null) Store.remove(this, pu.pcId);
            }
            closeAsync(tr);
        }
    }

    private void fail(final String msg) {
        Log.i(App.TAG, "配对失败：" + msg);
        runUi(new Runnable() {
            @Override
            public void run() {
                showDone(false, msg);
            }
        });
    }

    private void wipeCode() {
        if (pu != null) Arrays.fill(pu.code, (byte) 0);
    }

    private void showDone(final boolean ok, String msg) {
        progress.setVisibility(View.GONE);
        statusView.setTextColor(ok ? Ui.OK_GREEN : Ui.ERR_RED);
        statusView.setText(msg);
        action.setVisibility(View.VISIBLE);
        action.setText(ok ? "完成" : "返回");
        action.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent i = new Intent(PairActivity.this, MainActivity.class);
                i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
                startActivity(i);
                finish();
            }
        });
    }

    @Override
    public void status(final String text) {
        runUi(new Runnable() {
            @Override
            public void run() {
                statusView.setTextColor(Ui.TEXT);
                statusView.setText(text);
            }
        });
    }
}
