package com.trebledot.stone.runtime;

import android.app.Activity;
import android.graphics.Bitmap;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;

/** Opt-in developer snapshots from Stone's own software renderer, without desktop capture. */
final class ArtDiagnostics {
    private static final boolean ENABLED = Boolean.getBoolean("stone.diagnostics");
    private boolean captured;
    private boolean lastSecure;
    private long nextCheck;
    private final File directory;
    private volatile long lastFrame;
    private volatile boolean stopped;
    private Thread watchdog;

    ArtDiagnostics(Activity activity) {
        directory = new File("/data/stone-diagnostics", ArtProcess.diagnosticName(activity.getPackageName()));
    }

    void start() {
        if (!ENABLED || watchdog != null) return;
        lastFrame = android.os.SystemClock.uptimeMillis();
        Thread main = Thread.currentThread();
        watchdog = new Thread(() -> {
            int samples = 0;
            while (!stopped && samples < 120) {
                try { Thread.sleep(1000); }
                catch (InterruptedException done) { return; }
                long delay = android.os.SystemClock.uptimeMillis() - lastFrame;
                if (stopped || delay < 1000) continue;
                try {
                    if (!directory.isDirectory() && !directory.mkdirs()) continue;
                    try (Writer output = new OutputStreamWriter(new FileOutputStream(new File(directory, "stalls.txt"), true), StandardCharsets.UTF_8)) {
                        output.write("\nuptime=" + android.os.SystemClock.uptimeMillis() + " frameDelayMs=" + delay + "\n");
                        for (StackTraceElement frame : main.getStackTrace()) output.write("  " + frame + "\n");
                    }
                    if (samples == 0) {
                        try (Writer output = new OutputStreamWriter(new FileOutputStream(new File(directory, "stalled-threads.txt")), StandardCharsets.UTF_8)) {
                            for (java.util.Map.Entry<Thread, StackTraceElement[]> entry : Thread.getAllStackTraces().entrySet()) {
                                output.write("\n" + entry.getKey().getName() + " " + entry.getKey().getState() + "\n");
                                for (StackTraceElement frame : entry.getValue()) output.write("  " + frame + "\n");
                            }
                        }
                    }
                    samples++;
                } catch (java.io.IOException | RuntimeException error) {
                    Log.w("StoneDiagnostics", "Could not sample software frame stall", error);
                    return;
                }
            }
        }, "Stone frame watchdog");
        watchdog.setDaemon(true);
        watchdog.start();
    }

    void close() {
        stopped = true;
        if (watchdog != null) watchdog.interrupt();
    }

    void capture(Activity activity, Bitmap bitmap) {
        if (!ENABLED) return;
        lastFrame = android.os.SystemClock.uptimeMillis();
        long now = android.os.SystemClock.uptimeMillis();
        if (now < nextCheck) return;
        nextCheck = now + 1000;
        File request = new File(directory, "capture.request");
        File threads = new File(directory, "threads.request");
        boolean secure = ArtServices.windows.captureSecure(activity);
        if (captured && secure == lastSecure && !request.isFile() && !threads.isFile()) return;
        try {
            if (!directory.isDirectory() && !directory.mkdirs()) throw new java.io.IOException("Cannot create diagnostic directory");
            File stage = new File(directory, "frame.tmp.png");
            writeFrame(stage,bitmap,secure);
            if (!stage.renameTo(new File(directory, "frame.png"))) throw new java.io.IOException("Cannot publish diagnostic frame");
            File state = new File(directory,"state.tmp");
            try (Writer output = new OutputStreamWriter(new FileOutputStream(state), StandardCharsets.UTF_8)) {
                if (secure) output.write("capture=redacted\nreason=FLAG_SECURE\n");
                else {
                    output.write("activity=" + activity.getClass().getName() + "\n");
                    output.write("source=" + activity.getApplicationInfo().sourceDir + "\n");
                    output.write("uid=" + activity.getApplicationInfo().uid + "\n");
                    output.write("processStateSummaryBase64=" + ArtProcess.diagnosticStateSummary() + "\n");
                    output.write("applicationClassLoader=" + activity.getApplication().getClass().getClassLoader() + "\n");
                    output.write("activityClassLoader=" + activity.getClass().getClassLoader() + "\n");
                    android.content.Intent launch = ArtServices.activities.retainedIntent(activity);
                    output.write("intent=" + (launch == null ? "" : launch.toUri(android.content.Intent.URI_INTENT_SCHEME)) + "\n");
                    output.write("locales=" + activity.getResources().getConfiguration().getLocales().toLanguageTags() + "\n");
                    output.write("configuration=" + activity.getResources().getConfiguration() + "\n");
                    output.write("defaultLocale=" + java.util.Locale.getDefault().toLanguageTag() + "\n");
                    dump(activity.getWindow().getDecorView(), output, 0);
                }
            }
            if (!state.renameTo(new File(directory,"state.txt"))) throw new java.io.IOException("Cannot publish diagnostic state");
            captured = true;
            lastSecure = secure;
            if (threads.isFile()) {
                try (Writer output = new OutputStreamWriter(new FileOutputStream(new File(directory, "threads.txt")), StandardCharsets.UTF_8)) {
                    for (java.util.Map.Entry<Thread, StackTraceElement[]> entry : Thread.getAllStackTraces().entrySet()) {
                        output.write("\n" + entry.getKey().getName() + " " + entry.getKey().getState() + "\n");
                        for (StackTraceElement frame : entry.getValue()) output.write("  " + frame + "\n");
                    }
                }
                if (!threads.delete()) Log.w("StoneDiagnostics", "Could not clear thread dump request");
            }
            if (request.exists() && !request.delete()) Log.w("StoneDiagnostics", "Could not clear capture request");
        } catch (java.io.IOException | RuntimeException error) {
            Log.w("StoneDiagnostics", "Could not capture software frame", error);
        }
    }

    private static void writeFrame(File file, Bitmap live, boolean secure) throws java.io.IOException {
        Bitmap image = live;
        if (secure) {
            image = Bitmap.createBitmap(live.getWidth(),live.getHeight(),Bitmap.Config.ARGB_8888);
            image.eraseColor(android.graphics.Color.BLACK);
        }
        try (FileOutputStream output = new FileOutputStream(file)) {
            if (!image.compress(Bitmap.CompressFormat.PNG,100,output)) throw new java.io.IOException("PNG encoding failed");
        } finally {
            if (image != live) image.recycle();
        }
    }

    private static void dump(View view, Writer output, int depth) throws java.io.IOException {
        if (view.getVisibility() != View.VISIBLE || depth > 64) return;
        String text = view instanceof TextView ? String.valueOf(((TextView) view).getText()) : String.valueOf(view.getContentDescription());
        int[] location = new int[2];
        view.getLocationOnScreen(location);
        output.write(depth + " " + view.getClass().getName() + " id=" + view.getId()
            + " bounds=" + location[0] + "," + location[1] + "," + view.getWidth() + "," + view.getHeight()
            + " clickable=" + view.isClickable() + " enabled=" + view.isEnabled()
            + " focused=" + view.isFocused() + " windowFocus=" + view.hasWindowFocus() + " text="
            + text.replace('\n', ' ').replace('\r', ' ') + "\n");
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++) dump(group.getChildAt(index), output, depth + 1);
        }
    }
}
