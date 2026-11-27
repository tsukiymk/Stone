package com.trebledot.stone.probe;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;

/** Reproduces APK crash handlers which keep a nested main Looper alive. */
public final class SessionExitFixture extends Activity {
    private static String mode;
    public static final class App extends Application {
        @Override public void onCreate() {
            super.onCreate();
            try {
                mode = new String(Files.readAllBytes(new File(getFilesDir(), "session-exit-mode").toPath()),
                    StandardCharsets.UTF_8).trim();
            } catch (java.io.IOException error) { throw new AssertionError(error); }
            Thread worker = new Thread(() -> {
                try { new CountDownLatch(1).await(); }
                catch (InterruptedException error) { throw new AssertionError(error); }
            }, "Session exit non-daemon fixture");
            worker.setDaemon(false);
            worker.start();
            new Handler(Looper.getMainLooper()).post(() -> {
                Log.i("StoneSessionExit", "session-exit ready: " + mode);
                if ("plain".equals(mode)) return;
                for (;;) {
                    try { Looper.loop(); }
                    catch (Throwable error) { Log.e("StoneSessionExit", "session-exit caught Throwable", error); }
                }
            });
        }
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Stone session exit: " + mode);
        setContentView(text);
    }
    @Override protected void onDestroy() {
        super.onDestroy();
        try {
            Class<?> work = Class.forName("android.app.QueuedWork");
            work.getMethod("queue", Runnable.class, boolean.class).invoke(null, (Runnable) () -> {
                try (FileOutputStream output = new FileOutputStream(new File(getFilesDir(), "queued-work-finished"))) {
                    Thread.sleep(100);
                    output.write("drained".getBytes(StandardCharsets.UTF_8));
                    output.getFD().sync();
                } catch (Exception error) { throw new AssertionError(error); }
            }, true);
            if ("cleanup-failure".equals(mode)) {
                work.getMethod("addFinisher", Runnable.class).invoke(null, (Runnable) () -> {
                    throw new IllegalStateException("session-exit deliberate cleanup failure");
                });
            }
            Log.i("StoneSessionExit", "session-exit Activity destroyed");
        } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }
}
