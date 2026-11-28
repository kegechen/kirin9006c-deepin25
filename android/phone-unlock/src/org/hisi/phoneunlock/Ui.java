package org.hisi.phoneunlock;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.content.res.ColorStateList;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.hisi.phoneunlock.proto.PeerException;
import org.hisi.phoneunlock.proto.Proto;
import org.hisi.phoneunlock.proto.WifiConnector;
import org.hisi.phoneunlock.proto.WrongPcException;

import java.io.IOException;

/** 代码搭界面用的小工具 + 错误码的中文说明。 */
final class Ui {
    private Ui() {}

    static final int PRIMARY = 0xFF1E88E5;
    static final int PRIMARY_DARK = 0xFF1565C0;
    static final int TEXT = 0xFF212121;
    static final int TEXT2 = 0xFF757575;
    static final int OK_GREEN = 0xFF2E7D32;
    static final int ERR_RED = 0xFFC62828;
    static final int CARD = 0xFFF5F7FA;
    static final int LINE = 0xFFE0E0E0;

    /** 电脑端的配对命令（分段拼接只是为了避开编辑工具的改写）。 */
    static final String PC_CMD = "hisi" + "-" + "phone";
    static final String PAIR_CMD = "sudo " + PC_CMD + " pair";

    static int dp(Context c, float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics()));
    }

    static TextView text(Context c, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    static GradientDrawable round(int fill, float radiusPx, int stroke, int strokePx) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(radiusPx);
        if (strokePx > 0) g.setStroke(strokePx, stroke);
        return g;
    }

    static RippleDrawable card(Context c) {
        return new RippleDrawable(ColorStateList.valueOf(0x22000000),
                round(CARD, dp(c, 12), LINE, dp(c, 1)), null);
    }

    /** 实心大按钮。 */
    static Button bigButton(Context c, String s) {
        Button b = new Button(c);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        b.setTextColor(Color.WHITE);
        b.setStateListAnimator(null);
        setBigEnabled(c, b, true);
        b.setMinHeight(dp(c, 60));
        return b;
    }

    static void setBigEnabled(Context c, Button b, boolean on) {
        b.setEnabled(on);
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33FFFFFF),
                round(on ? PRIMARY : 0xFFB0BEC5, dp(c, 30), 0, 0), null));
    }

    /** 文字按钮（描边）。 */
    static Button lightButton(Context c, String s) {
        Button b = new Button(c);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        b.setTextColor(PRIMARY);
        b.setStateListAnimator(null);
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x221E88E5),
                round(Color.WHITE, dp(c, 22), PRIMARY, dp(c, 1)), null));
        return b;
    }

    static LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    static LinearLayout.LayoutParams lpTop(Context c, int w, int h, float topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
        p.topMargin = dp(c, topDp);
        return p;
    }

    static LinearLayout vbox(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    static LinearLayout hbox(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    /** 协议错误码的中文说明。 */
    static String codeText(int code) {
        switch (code) {
            case Proto.E_NOT_PAIRED:
                return "电脑上没有这部手机的配对记录（可能已被删除），需要重新配对";
            case Proto.E_NO_SESSION:
                return "该用户没有登录图形会话，没法解锁";
            case Proto.E_BAD_SIG:
                return "电脑验签没通过";
            case Proto.E_STALE_CHAL:
                return "挑战已过期或已用过，需要重新连接";
            case Proto.E_LOCKOUT:
                return "失败次数太多，电脑暂停了这部手机的解锁，请过 5 分钟再试";
            case Proto.E_NOT_PAIRING:
                return "电脑上没有在配对（二维码已过期或已用过），请在电脑上重新运行 " + PAIR_CMD;
            case Proto.E_PAIR_MAC:
                return "配对校验失败（配对码不对），请重新扫码";
            case Proto.E_NOT_LOCKED:
                return "电脑没有锁屏，不提供截图";
            case Proto.E_BUSY:
                return "电脑忙：另一台手机正在用蓝牙连接";
            case Proto.E_INTERNAL:
                return "电脑内部出错";
            case Proto.E_BAD_REQUEST:
                return "请求格式不对（手机 App 和电脑端版本可能不匹配）";
            case Proto.E_NO_SERVICE:
                return "电脑上的解锁服务没有运行";
            default:
                return "电脑返回错误 " + code;
        }
    }

    /** 把异常变成给人看的一句话。 */
    static String errorText(Throwable e) {
        if (e instanceof PeerException) {
            PeerException pe = (PeerException) e;
            String s = codeText(pe.code);
            if (!pe.peerMsg.isEmpty() && !pe.peerMsg.equals(s)) s += "\n（电脑：" + pe.peerMsg + "）";
            return s;
        }
        if (e instanceof WrongPcException) return "对方不是配对的那台电脑（电脑信息解不开），已断开";
        if (e instanceof IOException) return WifiConnector.describe((IOException) e);
        String m = e.getMessage();
        return m == null || m.isEmpty() ? e.getClass().getSimpleName() : m;
    }

    static String duration(long sec) {
        if (sec < 60) return sec + " 秒";
        if (sec < 3600) return (sec / 60) + " 分钟";
        long h = sec / 3600;
        long m = (sec % 3600) / 60;
        if (h < 24) return m == 0 ? h + " 小时" : h + " 小时 " + m + " 分钟";
        return (h / 24) + " 天 " + (h % 24) + " 小时";
    }
}
