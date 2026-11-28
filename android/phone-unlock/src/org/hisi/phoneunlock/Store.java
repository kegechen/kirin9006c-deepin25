package org.hisi.phoneunlock;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;

import java.util.ArrayList;
import java.util.List;

/** 配对记录和设置，存在 App 私有的 SharedPreferences 里（JSON）。 */
final class Store {
    private Store() {}

    static final String MODE_AUTO = "auto";
    static final String MODE_WIFI = "wifi";
    static final String MODE_BT = "bt";

    private static final String PREFS = "phone_unlock";
    private static final String K_PCS = "pcs";
    private static final String K_MODE = "mode";

    private static SharedPreferences prefs(Context c) {
        return c.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static synchronized List<PcRecord> list(Context c) {
        List<PcRecord> out = new ArrayList<>();
        String json = prefs(c).getString(K_PCS, "[]");
        try {
            JSONArray a = new JSONArray(json);
            for (int i = 0; i < a.length(); i++) {
                try {
                    out.add(PcRecord.fromJson(a.getJSONObject(i)));
                } catch (JSONException | IllegalArgumentException e) {
                    Log.w(App.TAG, "跳过一条坏的配对记录：" + e.getMessage());
                }
            }
        } catch (JSONException e) {
            Log.w(App.TAG, "配对记录读不出来：" + e.getMessage());
        }
        return out;
    }

    static synchronized PcRecord get(Context c, String pcId) {
        for (PcRecord r : list(c)) {
            if (r.pcId.equals(pcId)) return r;
        }
        return null;
    }

    static synchronized void put(Context c, PcRecord rec) {
        List<PcRecord> all = list(c);
        boolean replaced = false;
        for (int i = 0; i < all.size(); i++) {
            if (all.get(i).pcId.equals(rec.pcId)) {
                all.set(i, rec);
                replaced = true;
            }
        }
        if (!replaced) all.add(rec);
        save(c, all);
    }

    static synchronized void remove(Context c, String pcId) {
        List<PcRecord> all = list(c);
        List<PcRecord> keep = new ArrayList<>();
        for (PcRecord r : all) {
            if (!r.pcId.equals(pcId)) keep.add(r);
        }
        save(c, keep);
    }

    private static void save(Context c, List<PcRecord> all) {
        JSONArray a = new JSONArray();
        try {
            for (PcRecord r : all) a.put(r.toJson());
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        prefs(c).edit().putString(K_PCS, a.toString()).commit();
    }

    static String mode(Context c) {
        String m = prefs(c).getString(K_MODE, MODE_AUTO);
        return MODE_WIFI.equals(m) || MODE_BT.equals(m) ? m : MODE_AUTO;
    }

    static void setMode(Context c, String mode) {
        prefs(c).edit().putString(K_MODE, mode).apply();
    }
}
