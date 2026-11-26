package com.trebledot.stone.probe;

/** Independent Android services for real shared-ID ownership tests. */
public final class ForegroundFixture extends android.app.Activity {
    public static class First extends android.app.Service {
        @Override public android.os.IBinder onBind(android.content.Intent intent) { return null; }
        @Override public int onStartCommand(android.content.Intent intent, int flags, int id) { return START_NOT_STICKY; }
    }
    public static final class Second extends First { }
}
