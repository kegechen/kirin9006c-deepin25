package org.hisi.phoneunlock;

import android.content.Context;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import java.io.BufferedReader;
import java.io.InputStreamReader;

/**
 * 配对时自动取手机的名字，电脑锁屏和 hisi-phone list 里显示这个名字。
 * 认证型号 Build.MODEL（形如 23127PN0CC）用户看不懂，只在别的都取不到时才用。
 *
 * 顺序：
 *  1. 设备名称：用户在 设置 → 关于手机 里看到、能改的名字（华为 unified_device_name，其它厂商 device_name，再不行用蓝牙名）；
 *  2. 设备型号（营销名）：各厂商放在系统属性里的名字，如 nova 10z、Xiaomi 14；
 *  3. 认证型号 Build.MODEL。
 */
final class DeviceName {
    private DeviceName() {}

    /** 设备名称，按可信度排：华为/荣耀统一设备名 → 系统设备名 */
    private static final String[] GLOBAL_KEYS = {"unified_device_name", "device_name"};

    /** 厂商放营销名的系统属性：华为/荣耀、小米/红米、OPPO/一加/realme、vivo，以及 AOSP 通用的 marketname。 */
    private static final String[] PROPS = {
        "ro.config.marketing_name",
        "ro.product.marketname",
        "ro.product.vendor.marketname",
        "ro.product.odm.marketname",
        "ro.vendor.oplus.market.name",
        "ro.oppo.market.name",
        "ro.vivo.market.name",
    };

    private static String cached;

    static synchronized String get(Context ctx) {
        if (cached != null) return cached;
        String model = Build.MODEL == null ? "" : Build.MODEL.trim();

        for (String key : GLOBAL_KEYS) {
            String v = setting(ctx, true, key);
            if (usable(v, model)) {
                Log.i(App.TAG, "手机名取自设备名称 " + key + "：" + v);
                return cached = v.trim();
            }
        }
        for (String key : PROPS) {
            String v = prop(key);
            if (usable(v, model)) {
                Log.i(App.TAG, "手机名取自设备型号 " + key + "：" + v);
                return cached = v.trim();
            }
        }
        String bt = setting(ctx, false, "bluetooth_name");
        if (usable(bt, model)) {
            Log.i(App.TAG, "手机名取自蓝牙名称：" + bt);
            return cached = bt.trim();
        }
        String m = model.isEmpty() ? "Android" : model;
        Log.i(App.TAG, "没取到设备名称和设备型号，用认证型号：" + m);
        return cached = m;
    }

    /** 有内容、不是 generic 之类的占位值、也不只是认证型号本身（很多手机的设备名称默认就是认证型号）。 */
    private static boolean usable(String v, String model) {
        if (v == null) return false;
        String t = v.trim();
        return !t.isEmpty() && !t.toLowerCase().startsWith("generic") && !t.equalsIgnoreCase(model);
    }

    private static String setting(Context ctx, boolean global, String key) {
        try {
            return global ? Settings.Global.getString(ctx.getContentResolver(), key)
                    : Settings.Secure.getString(ctx.getContentResolver(), key);
        } catch (RuntimeException e) {
            Log.w(App.TAG, "读系统设置 " + key + " 失败：" + e);
            return null;
        }
    }

    private static boolean reflectionBlocked;

    /** 读系统属性：先反射 SystemProperties，不行再 getprop。 */
    private static String prop(String key) {
        if (!reflectionBlocked) {
            try {
                Class<?> sp = Class.forName("android.os.SystemProperties");
                return (String) sp.getMethod("get", String.class).invoke(null, key);
            } catch (Exception | LinkageError e) {
                reflectionBlocked = true;
                Log.i(App.TAG, "反射读系统属性不行，改用 getprop：" + e);
            }
        }
        Process p = null;
        try {
            p = new ProcessBuilder("getprop", key).redirectErrorStream(true).start();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), "UTF-8"));
            return r.readLine();
        } catch (Exception e) {
            return null;
        } finally {
            if (p != null) p.destroy();
        }
    }
}
