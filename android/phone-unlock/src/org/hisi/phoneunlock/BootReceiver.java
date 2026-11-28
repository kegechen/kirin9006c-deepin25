package org.hisi.phoneunlock;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** 开机、App 更新后：有电脑开着离开自动锁屏就把服务拉起来。 */
public final class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent i) {
        String a = i.getAction();
        if (Intent.ACTION_BOOT_COMPLETED.equals(a) || Intent.ACTION_MY_PACKAGE_REPLACED.equals(a)) {
            AwayService.sync(c);
        }
    }
}
