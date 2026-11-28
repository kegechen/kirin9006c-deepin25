package org.hisi.phoneunlock;

import android.app.Application;
import android.util.Log;

import org.hisi.phoneunlock.proto.Logger;

public class App extends Application {
    public static final String TAG = "PhoneUnlock";

    @Override
    public void onCreate() {
        super.onCreate();
        Logger.setSink(new Logger.Sink() {
            @Override
            public void log(String msg) {
                Log.i(TAG, msg);
            }
        });
    }
}
