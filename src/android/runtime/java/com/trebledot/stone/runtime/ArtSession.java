package com.trebledot.stone.runtime;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** One Application and main Looper, with an optional Windows Activity window. */
final class ArtSession {
    private static native int commands();
    static native boolean retire();
    private static native void complete(int result);
    private final ArtApplication application;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "Stone session"); thread.setDaemon(true); return thread;
    });
    private final Runnable pulse = this::pulse;
    private ArtWindowLoop window;
    private volatile boolean visible;
    private boolean retiring, shutdown;
    private long idleSince = SystemClock.uptimeMillis(), shutdownDeadline;
    private long grace;

    private ArtSession(ArtApplication application, boolean background) {
        this.application = application; grace = background ? 5000 : 1000;
    }
    static void run(ArtApplication application, boolean background) throws ReflectiveOperationException {
        ArtSession session = new ArtSession(application, background);
        try {
            if (!background) session.show();
            Log.i("StoneSession", background ? "Background Application ready; no launcher Activity" : "Foreground Application ready");
            session.main.post(session.pulse); Looper.loop();
        } finally { session.close(); }
    }
    private void close() throws ReflectiveOperationException {
        main.removeCallbacks(pulse); worker.shutdownNow(); closeWindow();
    }
    private void finish() {
        Throwable failure = null;
        try { close(); }
        catch (Throwable error) { failure = error; }
        try { ArtBoot.finishPendingWrites(); }
        catch (Throwable error) { if (failure == null) failure = error; else failure.addSuppressed(error); }
        if (failure != null) Log.e("StoneSession", "ART session cleanup failed", failure);
        // APKs may keep Looper.loop() inside catch(Throwable) indefinitely.
        // After cleanup, complete through the Client's normal native exit path.
        complete(failure == null ? 0 : 1);
        throw new IllegalStateException("Client session completion returned");
    }
    private void show() throws ReflectiveOperationException {
        if (window != null) return;
        Activity activity = application.currentActivity(null);
        if (activity == null) activity = application.startActivity();
        if (activity.isFinishing()) return;
        float density = activity.getResources().getDisplayMetrics().density;
        window = new ArtWindowLoop(activity, Math.max(1, Math.round(360 * density)), Math.max(1, Math.round(640 * density)));
        visible = true; window.start(() -> {
            try { closeWindow(); }
            catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot stop Activity", error); }
            grace = 1000; idleSince = SystemClock.uptimeMillis(); Log.i("StoneSession", "Window closed; Application remains available for background work");
        });
    }
    private void closeWindow() throws ReflectiveOperationException {
        ArtWindowLoop previous = window; window = null; visible = false;
        try { if (previous != null) previous.close(); }
        finally { application.destroyActivity(); }
    }
    private boolean canRetire() {
        return !visible && ArtServices.activities.importance() >= android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED
            && !ArtServices.serviceHost.hasWork() && !ArtServices.power.hasRequests() && !ArtServices.pendingIntents.hasWork()
            && !ArtServices.activities.hasExternalWork() && !ArtConfiguration.hasPendingSave() && !ArtAppearance.hasWork();
    }
    private void pulse() {
        try {
            if (!retiring) {
                int command = commands();
                if (command == 2 && !shutdown) {
                    shutdown = true; shutdownDeadline = SystemClock.uptimeMillis() + 5000;
                    closeWindow(); ArtServices.jobs.stopForShutdown();
                } else if (command == 1 && !shutdown) show();
            }
            if (shutdown) {
                if (ArtServices.jobs.idle() || SystemClock.uptimeMillis() >= shutdownDeadline) { finish(); return; }
            } else if (!retiring) {
                if (window == null && application.currentActivity(null) != null) show();
                if (!canRetire()) idleSince = SystemClock.uptimeMillis();
                else if (SystemClock.uptimeMillis() - idleSince >= grace) {
                    retiring = true;
                    worker.execute(() -> {
                        boolean granted = false;
                        try { if (canRetire()) granted = ArtServices.pendingIntents.retireIfIdle(); }
                        catch (RuntimeException error) { Log.w("StoneSession", "Idle retirement unavailable", error); }
                        final boolean exit = granted;
                        main.post(() -> {
                            if (exit) { Log.i("StoneSession", "Idle Client retirement granted"); finish(); return; }
                            retiring = false; idleSince = SystemClock.uptimeMillis();
                        });
                    });
                }
            }
        } catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot update ART session", error); }
        main.postDelayed(pulse, 100);
    }
}
