package com.trebledot.stone.probe;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.io.FileInputStream;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.channels.FileLock;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;

/** Real store transactions with delayed observation, missing history and a legacy writer. */
final class PackageEventsProbe {
    private static final String PACKAGE = "com.trebledot.stone.install.removal";
    private static final String INDEX = "/data/system/stone-packages/packages.properties";
    private static Object observer(ArtApplication running) throws Exception {
        Field field = ArtApplication.class.getDeclaredField("packageEvents"); field.setAccessible(true); return field.get(running);
    }
    static void flush(ArtApplication running) throws Exception {
        Object observer = observer(running);
        Method poll = observer.getClass().getDeclaredMethod("poll"); poll.setAccessible(true); poll.invoke(observer);
        BroadcastProbe.drain();
    }
    private static void edit(Object store, boolean legacy) throws Exception {
        Class<?> type = store.getClass();
        Field monitor = type.getDeclaredField("PROCESS_LOCK"); monitor.setAccessible(true);
        Method save = type.getDeclaredMethod("save", Properties.class); save.setAccessible(true);
        synchronized (monitor.get(null)) {
            try (RandomAccessFile registry = new RandomAccessFile("/data/system/stone-packages/registry.lock", "rw");
                    FileLock lock = registry.getChannel().lock()) {
                Properties state = new Properties();
                try (FileInputStream input = new FileInputStream(INDEX)) { state.load(input); }
                long sequence = Long.parseLong(state.getProperty("change.sequence"));
                state.remove("change.event." + sequence);
                if (legacy) state.setProperty("change.sequence", Long.toString(sequence - 1));
                save.invoke(store, state);
            }
        }
    }
    static void run(ArtApplication running) throws Exception {
        String fixtures = System.getProperty("stone.probe.archives"); if (fixtures == null) return;
        flush(running);
        Object observer = observer(running);
        Class<?> type = Class.forName("com.trebledot.stone.runtime.ArtPackageStore");
        java.lang.reflect.Constructor<?> constructor = type.getDeclaredConstructor(ArtPackage.class); constructor.setAccessible(true);
        Object store = constructor.newInstance(running.apk);
        Method install = type.getDeclaredMethod("install", Context.class, String.class, String.class, String.class); install.setAccessible(true);
        Method uninstall = type.getDeclaredMethod("uninstall", String.class, String.class); uninstall.setAccessible(true);
        List<Intent> events = new ArrayList<>();
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                if (intent.getData() != null && PACKAGE.equals(intent.getData().getSchemeSpecificPart())) events.add(new Intent(intent));
            }
        };
        IntentFilter filter = new IntentFilter(Intent.ACTION_PACKAGE_ADDED); filter.addDataScheme("package");
        filter.addAction(Intent.ACTION_PACKAGE_REMOVED); filter.addAction(Intent.ACTION_PACKAGE_REPLACED); filter.addAction(Intent.ACTION_PACKAGE_FULLY_REMOVED);
        running.context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        try {
            try { running.context.sendBroadcast(new Intent(Intent.ACTION_PACKAGE_ADDED)); throw new AssertionError("App forged a protected package broadcast"); }
            catch (SecurityException expected) { }
            PackageInfo first = (PackageInfo) install.invoke(store, running.context, fixtures + "/uninstall.apk", running.context.getPackageName(), PACKAGE);
            CommitProbe.await(() -> events.size() == 1);
            PackageInfo next;
            synchronized (observer) {
                next = (PackageInfo) install.invoke(store, running.context, fixtures + "/uninstall-update.apk", running.context.getPackageName(), PACKAGE);
                edit(store, false); // Committed APK remains valid; its retained notification row was lost.
                flush(running);
            }
            CommitProbe.await(() -> events.size() == 4);
            if (next.applicationInfo.uid != first.applicationInfo.uid || !events.get(1).getBooleanExtra(Intent.EXTRA_REPLACING, false)
                    || !Intent.ACTION_PACKAGE_REPLACED.equals(events.get(3).getAction())) throw new AssertionError("Journal gap did not reconcile an actual upgrade");
            synchronized (observer) {
                next = (PackageInfo) install.invoke(store, running.context, fixtures + "/uninstall-update.apk", running.context.getPackageName(), PACKAGE);
                edit(store, true); // Same-index schema and sequence, as written by an older runtime.
                flush(running);
            }
            CommitProbe.await(() -> events.size() == 7);
            uninstall.invoke(store, PACKAGE, next.applicationInfo.sourceDir);
            CommitProbe.await(() -> events.size() == 9);
            long until = android.os.SystemClock.uptimeMillis() + 750;
            while (android.os.SystemClock.uptimeMillis() < until) { BroadcastProbe.drain(); Thread.sleep(5); }
            if (events.size() != 9 || !Intent.ACTION_PACKAGE_REMOVED.equals(events.get(7).getAction())
                    || events.get(7).getBooleanExtra(Intent.EXTRA_REPLACING, true)
                    || !Intent.ACTION_PACKAGE_FULLY_REMOVED.equals(events.get(8).getAction())) throw new AssertionError("Removal event replay or ordering");
            System.out.println("ART durable package changes: gap reconciliation, older-writer detection, protected sender, ordered delivery and no replay passed.");
        } finally { running.context.unregisterReceiver(receiver); }
    }
}
