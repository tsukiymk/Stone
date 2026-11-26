package com.trebledot.stone.runtime;

import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

/** Rasterizes the currently enabled launcher without holding up the application's main Looper. */
final class ArtAppearance {
    private static ArtPackage apk;
    private static final AtomicInteger generation = new AtomicInteger();
    private static final AtomicInteger published = new AtomicInteger();
    private static final AtomicInteger pending = new AtomicInteger();
    private static final ExecutorService worker = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "Stone application icon"); thread.setDaemon(true); return thread;
    });
    private ArtAppearance() { }
    static void install(ArtPackage value) {
        apk = value;
        Runtime.getRuntime().addShutdownHook(new Thread(ArtAppearance::flushAtExit,"Stone application icon shutdown"));
    }
    static boolean hasWork() { return pending.get() != 0; }

    static void changed() {
        if (apk == null) return;
        int revision = generation.incrementAndGet();
        pending.incrementAndGet();
        // Apps commonly enable an alias and then disable its predecessor in
        // one main-thread call. Resolve after that whole operation completes.
        boolean posted = new Handler(Looper.getMainLooper()).post(() -> worker.execute(() -> {
            try {
                publish(revision,true);
            } catch (Exception | LinkageError error) {
                Log.w("StoneWindow", "Could not update the application icon", error);
            } finally { pending.decrementAndGet(); }
        }));
        if (!posted) pending.decrementAndGet();
    }

    private static void publish(int revision, boolean updateWindow) throws Exception {
        if (revision != generation.get() || revision == published.get()) return;
        ActivityInfo launcher = apk.launcher();
        if (launcher == null) { published.set(revision); return; }
        byte[] png = render(apk.resources,launcher.getIconResource());
        if (revision != generation.get()) return;
        String label = label(apk.resources,launcher);
        if (updateWindow && ArtWindow.isOpen()) ArtWindow.updateIcon(png);
        String[] paths = ArtShell.appearance(apk.applicationInfo.packageName,label,png);
        published.set(revision);
        if (updateWindow && revision == generation.get() && ArtWindow.isOpen()) ArtWindow.updateAppearance(label,paths[0],paths[1]);
    }

    private static void flushAtExit() {
        if (generation.get() == published.get()) return;
        java.util.concurrent.Future<?> task = null;
        try {
            // System.exit may hold the main Looper before changed()'s post ran.
            // Resolve the final alias on the existing worker, without window calls
            // or waiting for the exiting main thread to dispatch that post.
            task = worker.submit(() -> { publish(generation.get(),false); return null; });
            task.get(5,java.util.concurrent.TimeUnit.SECONDS);
        } catch (Exception error) {
            if (task != null) task.cancel(true);
            if (error instanceof InterruptedException) Thread.currentThread().interrupt();
            Log.w("StoneWindow","Could not publish the final application icon before exit",error);
        }
    }

    static String label(android.content.res.Resources resources, ActivityInfo launcher) {
        android.content.pm.PackageItemInfo info = launcher;
        if (info.nonLocalizedLabel == null && info.labelRes == 0) info = launcher.applicationInfo;
        if (info.nonLocalizedLabel != null) return info.nonLocalizedLabel.toString();
        return info.labelRes == 0 ? launcher.packageName : resources.getText(info.labelRes).toString();
    }

    static byte[] render(android.content.res.Resources resources, int resource) {
        return render(resources, resource, 256);
    }

    static byte[] settingsIcon(android.content.pm.PackageInfo info) {
        try {
            return ArtArchive.withResources(info, java.util.Locale.getDefault().toLanguageTag(), resources -> {
                byte[] png = render(resources, info.applicationInfo.icon, 96);
                if (png.length > 32768) png = render(resources, info.applicationInfo.icon, 64);
                if (png.length > 32768) throw new IllegalArgumentException("Settings icon exceeds its size limit");
                return png;
            });
        } catch (Exception | LinkageError error) {
            Log.w("StonePackages", "Cannot read application icon: " + info.packageName, error);
            return new byte[0];
        }
    }

    private static byte[] render(android.content.res.Resources resources, int resource, int size) {
        if (resource == 0) throw new IllegalArgumentException("Application has no icon resource");
        Drawable icon = resources.getDrawable(resource, resources.newTheme()).mutate();
        Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        try {
            Canvas canvas = new Canvas(bitmap);
            int width = icon.getIntrinsicWidth(), height = icon.getIntrinsicHeight();
            if (width > 0 && height > 0) {
                float scale = (float) size / Math.max(width, height);
                width = Math.max(1, Math.round(width * scale)); height = Math.max(1, Math.round(height * scale));
            } else { width = size; height = size; }
            int left = (size - width) / 2, top = (size - height) / 2;
            icon.setBounds(left, top, left + width, top + height);
            icon.draw(canvas); canvas.setBitmap(null);
            ByteArrayOutputStream png = new ByteArrayOutputStream();
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, png)) throw new IllegalStateException("Could not encode launcher icon");
            return png.toByteArray();
        } finally { bitmap.recycle(); }
    }
}
