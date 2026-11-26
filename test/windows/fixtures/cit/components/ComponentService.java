package com.trebledot.stone.cit.components;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Looper;

public class ComponentService extends Service {
    static int creates, starts, binds, unbinds, rebinds, destroys, firstStart, latestStart;
    static boolean allOnMain = true;
    private final Binder binder = new LocalBinder();

    private static void checkThread() {
        allOnMain &= Looper.myLooper() == Looper.getMainLooper();
    }

    final class LocalBinder extends Binder {
        ComponentService service() { return ComponentService.this; }
    }

    @Override public void onCreate() { checkThread(); creates++; }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        checkThread();
        if (++starts == 1) firstStart = startId;
        latestStart = startId;
        return START_NOT_STICKY;
    }
    @Override public IBinder onBind(Intent intent) {
        checkThread();
        binds++;
        return intent.getBooleanExtra("null", false) ? null : binder;
    }
    @Override public boolean onUnbind(Intent intent) { checkThread(); unbinds++; return !intent.getBooleanExtra("no-rebind", false); }
    @Override public void onRebind(Intent intent) { checkThread(); rebinds++; }
    @Override public void onDestroy() { checkThread(); destroys++; }

    public static final class Disabled extends ComponentService {
        @Override public void onCreate() { throw new AssertionError("Disabled service ran"); }
    }
    public static final class Remote extends ComponentService {
        @Override public void onCreate() { throw new AssertionError("Remote service ran in the local process"); }
    }
    public static final class Isolated extends ComponentService {
        @Override public void onCreate() { throw new AssertionError("Isolated service ran in the local process"); }
    }
}
