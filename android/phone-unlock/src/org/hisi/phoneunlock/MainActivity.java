package org.hisi.phoneunlock;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.hisi.phoneunlock.proto.HostPort;
import org.hisi.phoneunlock.proto.PairUri;

import java.util.ArrayList;
import java.util.List;

/** 主界面：连接方式 + 已配对电脑列表。 */
public class MainActivity extends Activity {
    private static final int M_SCAN = 1;
    private static final int M_PASTE = 2;
    private static final int M_DELETE = 3;
    private static final int M_EXTRA = 4;
    private static final int M_ABOUT = 5;

    private LinearLayout list;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        int pad = Ui.dp(this, 16);

        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        LinearLayout col = Ui.vbox(this);
        col.setPadding(pad, pad, pad, pad);
        sv.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        col.addView(Ui.text(this, "连接方式", 14, Ui.TEXT2, false));
        RadioGroup rg = new RadioGroup(this);
        rg.setOrientation(RadioGroup.HORIZONTAL);
        String[] labels = {"自动", "Wi-Fi", "蓝牙"};
        final String[] modes = {Store.MODE_AUTO, Store.MODE_WIFI, Store.MODE_BT};
        String cur = Store.mode(this);
        for (int i = 0; i < 3; i++) {
            RadioButton r = new RadioButton(this);
            r.setId(100 + i);
            r.setText(labels[i]);
            r.setTextSize(16);
            r.setPadding(0, 0, Ui.dp(this, 20), 0);
            rg.addView(r);
            if (modes[i].equals(cur)) r.setChecked(true);
        }
        rg.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(RadioGroup group, int id) {
                int i = id - 100;
                if (i >= 0 && i < 3) Store.setMode(MainActivity.this, modes[i]);
            }
        });
        col.addView(rg);
        col.addView(Ui.text(this, "自动：先找同一局域网（Wi-Fi），找不到再用蓝牙。", 12, Ui.TEXT2, false));

        TextView h = Ui.text(this, "已配对的电脑", 14, Ui.TEXT2, false);
        col.addView(h, Ui.lpTop(this, -2, -2, 24));
        list = Ui.vbox(this);
        col.addView(list, Ui.lpTop(this, -1, -2, 8));

        setContentView(sv);
    }

    @Override
    protected void onResume() {
        super.onResume();
        AwayService.setListener(new Runnable() {
            @Override
            public void run() {
                refresh();
            }
        });
        AwayService.sync(this);
        refresh();
    }

    @Override
    protected void onPause() {
        AwayService.setListener(null);
        super.onPause();
    }

    private void refresh() {
        list.removeAllViews();
        List<PcRecord> recs = Store.list(this);
        if (recs.isEmpty()) {
            TextView t = Ui.text(this, "还没有配对的电脑。\n\n在电脑上（已登录桌面时）运行\n" + Ui.PAIR_CMD
                    + "\n屏幕上会出现二维码，点下面的按钮扫码。", 15, Ui.TEXT, false);
            t.setGravity(Gravity.CENTER);
            t.setLineSpacing(0, 1.2f);
            list.addView(t, Ui.lpTop(this, -1, -2, 24));
            Button scan = Ui.bigButton(this, "扫码配对");
            scan.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    startActivity(new Intent(MainActivity.this, ScanActivity.class));
                }
            });
            list.addView(scan, Ui.lpTop(this, -1, -2, 28));
            Button paste = Ui.lightButton(this, "粘贴配对链接");
            paste.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    pasteDialog();
                }
            });
            list.addView(paste, Ui.lpTop(this, -1, Ui.dp(this, 48), 12));
            return;
        }
        for (final PcRecord r : recs) {
            LinearLayout card = Ui.vbox(this);
            int p = Ui.dp(this, 16);
            card.setPadding(p, p, p, p);
            card.setBackground(Ui.card(this));
            card.setClickable(true);
            card.addView(Ui.text(this, r.title(), 20, Ui.TEXT, true));
            String user = r.user.isEmpty() ? "（未知）" : r.user;
            card.addView(Ui.text(this, "用户：" + user, 15, Ui.TEXT, false), Ui.lpTop(this, -2, -2, 4));
            if (!r.extra.isEmpty()) {
                card.addView(Ui.text(this, "额外地址：" + r.extra, 13, Ui.TEXT2, false), Ui.lpTop(this, -2, -2, 2));
            }
            if (r.oneTap) {
                card.addView(Ui.text(this, "一键解锁（测试模式，不验证指纹）", 13, Ui.ERR_RED, false), Ui.lpTop(this, -2, -2, 2));
            }
            if (r.awayGrace > 0) {
                String st = AwayService.state(r.pcId);
                String line = "离开自动锁屏：" + Ui.duration(r.awayGrace) + (st.isEmpty() ? "" : " · " + st);
                card.addView(Ui.text(this, line, 13, Ui.OK_GREEN, false), Ui.lpTop(this, -2, -2, 2));
            }
            card.addView(Ui.text(this, "点按连接并解锁 · 长按更多操作", 13, Ui.PRIMARY, false), Ui.lpTop(this, -2, -2, 8));
            card.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    startActivity(new Intent(MainActivity.this, UnlockActivity.class).putExtra(UnlockActivity.EXTRA_PC, r.pcId));
                }
            });
            card.setOnLongClickListener(new View.OnLongClickListener() {
                @Override
                public boolean onLongClick(View v) {
                    pcActions(r);
                    return true;
                }
            });
            list.addView(card, Ui.lpTop(this, -1, -2, 8));
        }
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        menu.add(0, M_SCAN, 0, "配对新电脑（扫码）");
        menu.add(0, M_PASTE, 1, "粘贴配对链接");
        menu.add(0, M_DELETE, 2, "删除这台电脑");
        menu.add(0, M_EXTRA, 3, "编辑额外地址");
        menu.add(0, M_ABOUT, 4, "关于");
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        switch (item.getItemId()) {
            case M_SCAN:
                startActivity(new Intent(this, ScanActivity.class));
                return true;
            case M_PASTE:
                pasteDialog();
                return true;
            case M_DELETE:
                pickPc(new PcCallback() {
                    @Override
                    public void on(PcRecord r) {
                        confirmDelete(r);
                    }
                });
                return true;
            case M_EXTRA:
                pickPc(new PcCallback() {
                    @Override
                    public void on(PcRecord r) {
                        editExtra(r);
                    }
                });
                return true;
            case M_ABOUT:
                about();
                return true;
            default:
                return super.onOptionsItemSelected(item);
        }
    }

    /** 关于：显示应用版本（AndroidManifest 的 versionName）。 */
    private void about() {
        String v;
        try {
            v = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            v = "未知";
        }
        new AlertDialog.Builder(this)
                .setTitle("关于")
                .setMessage("电脑解锁\n版本 " + v + "\n\n"
                        + "在手机上验证后解锁电脑：同一局域网走 Wi-Fi（也支持蓝牙），"
                        + "装了 Tailscale 时可以在外面远程解锁（电脑地址填 100.x）。")
                .setPositiveButton("好", null)
                .show();
    }

    private interface PcCallback {
        void on(PcRecord r);
    }

    private void pickPc(final PcCallback cb) {
        final List<PcRecord> recs = Store.list(this);
        if (recs.isEmpty()) {
            Toast.makeText(this, "还没有配对的电脑", Toast.LENGTH_SHORT).show();
            return;
        }
        if (recs.size() == 1) {
            cb.on(recs.get(0));
            return;
        }
        String[] names = new String[recs.size()];
        for (int i = 0; i < names.length; i++) names[i] = recs.get(i).title() + "（" + recs.get(i).user + "）";
        new AlertDialog.Builder(this).setTitle("选择电脑").setItems(names, new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface d, int which) {
                cb.on(recs.get(which));
            }
        }).show();
    }

    private void pcActions(final PcRecord r) {
        String away = "离开自动锁屏：" + (r.awayGrace > 0 ? Ui.duration(r.awayGrace) : "关");
        new AlertDialog.Builder(this).setTitle(r.title())
                .setItems(new String[] {"一键锁屏", away, "编辑额外地址", "删除这台电脑"}, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        switch (which) {
                            case 0:
                                startActivity(new Intent(MainActivity.this, UnlockActivity.class)
                                        .putExtra(UnlockActivity.EXTRA_PC, r.pcId)
                                        .putExtra(UnlockActivity.EXTRA_LOCK, true));
                                break;
                            case 1:
                                awayDialog(r);
                                break;
                            case 2:
                                editExtra(r);
                                break;
                            default:
                                confirmDelete(r);
                        }
                    }
                }).show();
    }

    private static final int[] AWAY_CHOICES = {0, 15, 30, 60, 120, 300};
    private static final int REQ_AWAY_PERM = 51;
    /** 等权限框时记着要开的那台和秒数。 */
    private String pendingAwayPc;
    private int pendingAwayGrace;

    /** 离开自动锁屏（类似 Windows 的动态锁）：手机蓝牙断开后，电脑等几秒锁屏。 */
    private void awayDialog(final PcRecord r) {
        if (!BluetoothAdapter.checkBluetoothAddress(r.bt)) {
            Toast.makeText(this, "这台电脑的配对信息里没有蓝牙地址，用不了离开自动锁屏", Toast.LENGTH_LONG).show();
            return;
        }
        String[] names = new String[AWAY_CHOICES.length];
        int checked = 0;
        for (int i = 0; i < names.length; i++) {
            names[i] = AWAY_CHOICES[i] == 0 ? "关" : "断开 " + Ui.duration(AWAY_CHOICES[i]) + "后锁屏";
            if (AWAY_CHOICES[i] == r.awayGrace) checked = i;
        }
        String m = "开启后手机会在后台和电脑一直保持蓝牙连接（通知栏有常驻通知）。"
                + "手机走远、蓝牙断开后，电脑等设定的时间再锁屏；这段时间内手机回来连上就不锁。"
                + "只会锁、不会解锁；锁过一次要等手机重新连上才会再生效。\n"
                + "手机关蓝牙、App 被系统清掉也算离开。";
        if ("HUAWEI".equalsIgnoreCase(Build.MANUFACTURER) || "HONOR".equalsIgnoreCase(Build.MANUFACTURER)) {
            /* EMUI 默认不让侧载的 App 自启动：开机、App 更新、被清理后服务都起不来（实测更新后收不到 MY_PACKAGE_REPLACED） */
            m += "\n华为 / 荣耀手机：请到 设置 → 应用 → 应用启动管理，把“电脑解锁”改成手动管理并打开全部开关，"
                    + "否则手机重启、App 更新或被清理后要手动打开一次 App 才会恢复。";
        }
        TextView msg = Ui.text(this, m, 13, Ui.TEXT2, false);
        msg.setLineSpacing(0, 1.2f);
        int p = Ui.dp(this, 20);
        msg.setPadding(p, Ui.dp(this, 12), p, 0);
        new AlertDialog.Builder(this)
                .setTitle("离开自动锁屏 · " + r.title())
                .setSingleChoiceItems(names, checked, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        d.dismiss();
                        setAway(r.pcId, AWAY_CHOICES[which]);
                    }
                })
                .setView(msg)
                .setNegativeButton("取消", null)
                .show();
    }

    private void setAway(String pcId, int grace) {
        if (grace > 0) {
            List<String> need = new ArrayList<>();
            if (Build.VERSION.SDK_INT >= 31
                    && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                need.add(Manifest.permission.BLUETOOTH_CONNECT);
            }
            if (Build.VERSION.SDK_INT >= 33
                    && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                need.add(Manifest.permission.POST_NOTIFICATIONS);
            }
            if (!need.isEmpty()) {
                pendingAwayPc = pcId;
                pendingAwayGrace = grace;
                requestPermissions(need.toArray(new String[0]), REQ_AWAY_PERM);
                return;
            }
        }
        PcRecord cur = Store.get(this, pcId);
        if (cur == null) return;
        cur.awayGrace = grace;
        Store.put(this, cur);
        AwayService.sync(this);
        Toast.makeText(this, grace > 0 ? "已开启离开自动锁屏" : "已关闭离开自动锁屏", Toast.LENGTH_SHORT).show();
        refresh();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQ_AWAY_PERM || pendingAwayPc == null) return;
        String pc = pendingAwayPc;
        pendingAwayPc = null;
        if (Build.VERSION.SDK_INT >= 31
                && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, "没有“附近的设备”权限，开不了离开自动锁屏", Toast.LENGTH_LONG).show();
            return;
        }
        /* 通知权限没给也能开，只是看不到常驻通知 */
        PcRecord cur = Store.get(this, pc);
        if (cur == null) return;
        cur.awayGrace = pendingAwayGrace;
        Store.put(this, cur);
        AwayService.sync(this);
        Toast.makeText(this, "已开启离开自动锁屏", Toast.LENGTH_SHORT).show();
        refresh();
    }

    private void confirmDelete(final PcRecord r) {
        new AlertDialog.Builder(this)
                .setTitle("删除“" + r.title() + "”？")
                .setMessage("删除后要重新扫码配对才能再解锁。\n电脑上的配对记录可以在电脑上用 sudo " + Ui.PC_CMD + " unpair 删除。")
                .setPositiveButton("删除", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        KeyHelper.delete(r.alias());
                        Store.remove(MainActivity.this, r.pcId);
                        AwayService.sync(MainActivity.this);   /* 开着离开锁屏的话先发 AWAY 0 再断开 */
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void editExtra(final PcRecord r) {
        final EditText et = new EditText(this);
        et.setText(r.extra);
        et.setHint("例如 127.0.0.1, 192.168.1.10:47390");
        et.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        et.setSingleLine(true);
        LinearLayout box = Ui.vbox(this);
        int p = Ui.dp(this, 20);
        box.setPadding(p, Ui.dp(this, 8), p, 0);
        box.addView(Ui.text(this, "Wi-Fi 连接时会和二维码里的 IP 一起尝试。逗号分隔的 host[:port]，不写端口就用 "
                + r.port + "。", 13, Ui.TEXT2, false));
        box.addView(et, Ui.lpTop(this, -1, -2, 8));
        new AlertDialog.Builder(this)
                .setTitle("额外地址 · " + r.title())
                .setView(box)
                .setPositiveButton("保存", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        String text = et.getText().toString();
                        List<HostPort> l;
                        try {
                            l = HostPort.parseList(text, r.port);
                        } catch (IllegalArgumentException e) {
                            Toast.makeText(MainActivity.this, e.getMessage(), Toast.LENGTH_LONG).show();
                            return;
                        }
                        StringBuilder sb = new StringBuilder();
                        for (String part : text.split("[,，;；\\s]+")) {
                            if (part.trim().isEmpty()) continue;
                            if (sb.length() > 0) sb.append(", ");
                            sb.append(part.trim());
                        }
                        PcRecord cur = Store.get(MainActivity.this, r.pcId);
                        if (cur == null) return;
                        cur.extra = sb.toString();
                        Store.put(MainActivity.this, cur);
                        Toast.makeText(MainActivity.this, "已保存 " + l.size() + " 个地址", Toast.LENGTH_SHORT).show();
                        refresh();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void pasteDialog() {
        final EditText et = new EditText(this);
        et.setHint("hisiphone://pair?...");
        et.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        et.setMinLines(3);
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm != null && cm.hasPrimaryClip()) {
            ClipData cd = cm.getPrimaryClip();
            if (cd != null && cd.getItemCount() > 0) {
                CharSequence s = cd.getItemAt(0).coerceToText(this);
                if (s != null && PairUri.looksLike(s.toString())) et.setText(s.toString().trim());
            }
        }
        LinearLayout box = Ui.vbox(this);
        int p = Ui.dp(this, 20);
        box.setPadding(p, Ui.dp(this, 8), p, 0);
        box.addView(Ui.text(this, "把电脑上 " + Ui.PAIR_CMD + " 显示的配对链接粘贴到这里：", 13, Ui.TEXT2, false));
        box.addView(et, Ui.lpTop(this, -1, -2, 8));
        new AlertDialog.Builder(this)
                .setTitle("粘贴配对链接")
                .setView(box)
                .setPositiveButton("下一步", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        String s = et.getText().toString().trim();
                        if (!PairUri.looksLike(s)) {
                            Toast.makeText(MainActivity.this, "这不是配对链接（应以 hisiphone://pair? 开头）", Toast.LENGTH_LONG).show();
                            return;
                        }
                        startActivity(new Intent(MainActivity.this, PairActivity.class).setData(Uri.parse(s)));
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }
}
