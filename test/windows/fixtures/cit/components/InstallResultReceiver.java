package com.trebledot.stone.cit.components;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;
import java.util.List;

/** Real manifest receiver for image PackageInstaller status and goAsync verification. */
public final class InstallResultReceiver extends BroadcastReceiver {
    public static final List<Intent> received = new ArrayList<>();
    public static boolean valid = true;
    @Override public void onReceive(Context context, Intent intent) {
        valid &= Looper.myLooper() == Looper.getMainLooper();
        PendingResult pending = goAsync();
        valid &= pending != null;
        new Handler(Looper.getMainLooper()).post(() -> {
            received.add(new Intent(intent));
            if (pending != null) pending.finish();
        });
    }
}
