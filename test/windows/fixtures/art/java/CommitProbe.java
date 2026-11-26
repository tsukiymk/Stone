package com.trebledot.stone.probe;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import com.trebledot.stone.runtime.ArtApplication;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;

/** Drives public session commit and the real confirmation Activity with controlled fixtures. */
final class CommitProbe {
    private static final String PACKAGE = "com.trebledot.stone.install.other";
    private static final String RECEIVER = "com.trebledot.stone.cit.components.InstallResultReceiver";
    static final class Status implements AutoCloseable {
        final Context context;
        final List<Intent> received = new ArrayList<>();
        final PendingIntent pending;
        final BroadcastReceiver receiver;
        Status(Context context, int code, boolean manifest) {
            this.context = context;
            String action = context.getPackageName() + ".COMMIT_STATUS_" + code;
            receiver = new BroadcastReceiver() {
                @Override public void onReceive(Context context, Intent intent) { received.add(new Intent(intent)); }
            };
            context.registerReceiver(receiver, new IntentFilter(action), Context.RECEIVER_NOT_EXPORTED);
            Intent intent = new Intent(action).setPackage(context.getPackageName());
            if (manifest) intent.setComponent(new ComponentName(context.getPackageName(), RECEIVER));
            pending = PendingIntent.getBroadcast(context, code, intent, PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_CANCEL_CURRENT);
        }
        @Override public void close() { pending.cancel(); context.unregisterReceiver(receiver); }
    }

    interface Condition { boolean done() throws Exception; }
    static void await(Condition condition) throws Exception {
        long deadline = android.os.SystemClock.uptimeMillis() + 20000;
        do { BroadcastProbe.drain(); if (condition.done()) return; Thread.sleep(5); } while (android.os.SystemClock.uptimeMillis() < deadline);
        throw new AssertionError("install status or Activity timed out");
    }

    static void recover(Context context) throws Exception {
        if (System.getProperty("stone.probe.archives") == null) return;
        File marker = new File(context.getFilesDir(), "commit-recovery.properties");
        if (!marker.isFile()) return;
        Properties state = new Properties();
        try (FileInputStream input = new FileInputStream(marker)) { state.load(input); }
        int id = Integer.parseInt(state.getProperty("id"));
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        if (!context.getPackageManager().canRequestPackageInstalls()) throw new AssertionError("declared package install requests are not supported");
        if (installer.getSessionInfo(id) == null || !installer.getSessionInfo(id).isCommitted())
            throw new AssertionError("interrupted committed session was not restored");
        try (Status status = new Status(context, 5100, false); PackageInstaller.Session session = installer.openSession(id)) {
            session.commit(status.pending.getIntentSender());
            await(() -> status.received.size() == 1);
            if (status.received.get(0).getIntExtra(PackageInstaller.EXTRA_STATUS, -99) != PackageInstaller.STATUS_SUCCESS
                    || !context.getPackageManager().getPackageInfo(PACKAGE, 0).applicationInfo.sourceDir.equals(state.getProperty("source"))
                    || installer.getSessionInfo(id) != null)
                throw new AssertionError("install receipt recovery reinstalled or re-prompted");
        }
        if (!marker.delete()) throw new AssertionError("commit recovery marker cleanup");
        System.out.println("ART acknowledged an interrupted committed session from its durable receipt without reinstalling or asking for confirmation again.");
    }

    static void run(ArtApplication running) throws Exception {
        String root = System.getProperty("stone.probe.archives");
        if (root == null) return;
        Context context = running.context;
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageEventsProbe.flush(running); // Finish notifications from preceding fixture setup before registering this observer.
        List<Intent> changes = new ArrayList<>();
        BroadcastReceiver observer = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                if (intent.getData() != null && PACKAGE.equals(intent.getData().getSchemeSpecificPart())) changes.add(new Intent(intent));
            }
        };
        IntentFilter filter = new IntentFilter(Intent.ACTION_PACKAGE_ADDED);
        filter.addAction(Intent.ACTION_PACKAGE_REPLACED); filter.addDataScheme("package");
        context.registerReceiver(observer, filter, Context.RECEIVER_NOT_EXPORTED);
        Activity[] current = new Activity[1];
        android.app.Application.ActivityLifecycleCallbacks lifecycle = new android.app.Application.ActivityLifecycleCallbacks() {
            public void onActivityCreated(Activity activity, Bundle state) { }
            public void onActivityStarted(Activity activity) { }
            public void onActivityResumed(Activity activity) { current[0] = activity; }
            public void onActivityPaused(Activity activity) { }
            public void onActivityStopped(Activity activity) { }
            public void onActivitySaveInstanceState(Activity activity, Bundle state) { }
            public void onActivityDestroyed(Activity activity) { }
        };
        running.application.registerActivityLifecycleCallbacks(lifecycle);
        Activity launcher = running.startActivity();
        try {
            try (Status status = new Status(context, 5101, false)) {
                int id = stage(context, root, "other-package");
                try (PackageInstaller.Session session = installer.openSession(id)) {
                    try (OutputStream open = session.openWrite("base.apk", 0, -1)) {
                        try { session.commit(status.pending.getIntentSender()); throw new AssertionError("commit accepted an open write stream"); }
                        catch (SecurityException expected) { }
                    }
                    if (installer.getSessionInfo(id).isCommitted()) throw new AssertionError("rejected commit sealed the session");
                    session.commit(status.pending.getIntentSender());
                    await(() -> !status.received.isEmpty());
                    if (!installer.getSessionInfo(id).isCommitted()) throw new AssertionError("committed session was not sealed");
                    try {
                        installer.getClass().getMethod("setPermissionsResult", int.class, boolean.class).invoke(installer, id, true);
                        throw new AssertionError("installer bypassed the confirmation Activity");
                    } catch (java.lang.reflect.InvocationTargetException expected) {
                        if (!(expected.getCause() instanceof SecurityException)) throw expected;
                    }
                    try { session.openWrite("extra.apk", 0, -1); throw new AssertionError("sealed session remained writable"); }
                    catch (SecurityException expected) { }
                    try { session.commit(status.pending.getIntentSender()); throw new AssertionError("duplicate commit accepted"); }
                    catch (IllegalStateException expected) { }
                    Intent approved = status.received.get(0).getParcelableExtra(Intent.EXTRA_INTENT);
                    Intent forged = new Intent(approved).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    forged.removeExtra("com.trebledot.stone.install.NONCE");
                    try {
                        context.startActivity(forged);
                        throw new AssertionError("confirmation with no token was accepted");
                    } catch (android.content.ActivityNotFoundException expected) { }
                    show(context, status.received.get(0), current);
                    if (!current[0].findViewById(android.R.id.home).performClick()) throw new AssertionError("back navigation did not click");
                    await(() -> status.received.size() == 2 && current[0] == launcher);
                    if (status.received.get(1).getIntExtra(PackageInstaller.EXTRA_STATUS, -99) != PackageInstaller.STATUS_FAILURE_ABORTED
                            || installer.getSessionInfo(id) != null) throw new AssertionError("confirmation cancellation");
                }
            }
            int immutableId = stage(context, root, "other-package");
            PendingIntent immutable = PendingIntent.getBroadcast(context, 5106,
                new Intent("com.trebledot.stone.IMMUTABLE_STATUS").setComponent(new ComponentName(context.getPackageName(), RECEIVER)), PendingIntent.FLAG_IMMUTABLE);
            int targetSdk = running.apk.applicationInfo.targetSdkVersion;
            try (PackageInstaller.Session session = installer.openSession(immutableId)) {
                running.apk.applicationInfo.targetSdkVersion = 35;
                try { session.commit(immutable.getIntentSender()); throw new AssertionError("immutable status receiver accepted for SDK 35"); }
                catch (IllegalArgumentException expected) { }
            } finally { running.apk.applicationInfo.targetSdkVersion = targetSdk; immutable.cancel(); installer.abandonSession(immutableId); }
            if (!changes.isEmpty()) throw new AssertionError("cancelled install sent a package-added broadcast");
            try (Status status = new Status(context, 5102, false)) {
                int id = stage(context, root, "tampered-v2");
                try (PackageInstaller.Session session = installer.openSession(id)) { session.commit(status.pending.getIntentSender()); }
                await(() -> !status.received.isEmpty());
                if (status.received.size() != 1 || status.received.get(0).getIntExtra(PackageInstaller.EXTRA_STATUS, -99) != PackageInstaller.STATUS_FAILURE_INVALID)
                    throw new AssertionError("invalid APK commit did not fail before user confirmation");
            }
            PackageInfo installed = complete(context, root, "other-package", 5103, false, false, current, launcher);
            int uid = installed.applicationInfo.uid;
            await(() -> changes.size() == 1);
            if (changes.get(0).getBooleanExtra(Intent.EXTRA_REPLACING, true) || changes.get(0).getIntExtra(Intent.EXTRA_UID, 0) != uid)
                throw new AssertionError("new installation package broadcast");
            installed = complete(context, root, "other-update", 5104, true, true, current, launcher);
            await(() -> changes.size() == 3);
            if (installed.getLongVersionCode() != 8 || installed.applicationInfo.uid != uid
                    || !changes.get(1).getBooleanExtra(Intent.EXTRA_REPLACING, false)
                    || !Intent.ACTION_PACKAGE_REPLACED.equals(changes.get(2).getAction()))
                throw new AssertionError("upgrade commit and replacement broadcasts");
            complete(context, root, "other-bad-signer", 5105, false, false, current, launcher);
            if (changes.size() != 3) throw new AssertionError("failed signer replacement sent a package broadcast");
            await(() -> {
                for (Thread thread : Thread.getAllStackTraces().keySet()) if (thread.getName().startsWith("Stone APK commit ") && thread.isAlive()) return false;
                return true;
            });
        } finally {
            running.destroyActivity();
            running.application.unregisterActivityLifecycleCallbacks(lifecycle);
            context.unregisterReceiver(observer);
        }
        System.out.println("ART committed public install sessions through confirmation/cancel Activities, dynamic and manifest results, upgrades, conflicts and package broadcasts.");
    }

    private static PackageInfo complete(Context context, String root, String variant, int code, boolean manifest, boolean interrupt,
            Activity[] current, Activity launcher) throws Exception {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        Class<?> receiver = Class.forName(RECEIVER);
        @SuppressWarnings("unchecked") List<Intent> manifestResults = (List<Intent>) receiver.getField("received").get(null);
        int before = manifestResults.size();
        PackageInfo previous;
        try { previous = context.getPackageManager().getPackageInfo(PACKAGE,0); }
        catch (android.content.pm.PackageManager.NameNotFoundException absent) { previous = null; }
        try (Status status = new Status(context, code, manifest)) {
            List<Intent> results = manifest ? manifestResults : status.received;
            int start = manifest ? before : 0;
            int id = stage(context, root, variant);
            try (PackageInstaller.Session session = installer.openSession(id)) { session.commit(status.pending.getIntentSender()); }
            await(() -> results.size() > start);
            show(context, results.get(start), current);
            String language = "en";
            android.os.LocaleList locales = current[0].getResources().getConfiguration().getLocales();
            for (int index = 0; index < locales.size(); index++) {
                String candidate = locales.get(index).getLanguage();
                if (candidate.equals("en") || candidate.equals("zh") || candidate.equals("ja")) { language = candidate; break; }
            }
            boolean zh = language.equals("zh"), ja = language.equals("ja");
            boolean updating = previous != null;
            if (current[0].findViewById(android.R.id.button2) != null) throw new AssertionError("Unexpected cancel button");
            String expectedTitle = updating ? (zh ? "更新应用" : ja ? "アプリの更新" : "Update app") : (zh ? "安装应用" : ja ? "アプリのインストール" : "Install app");
            String expectedAction = updating ? (zh ? "更新" : ja ? "更新" : "Update") : (zh ? "安装" : ja ? "インストール" : "Install");
            if (!expectedTitle.contentEquals(current[0].getTitle())
                    || !expectedAction.contentEquals(((android.widget.Button)current[0].findViewById(android.R.id.button1)).getText()))
                throw new AssertionError("Install/update confirmation title or action differs");
            if (updating) {
                String oldVersion = previous.versionName == null || previous.versionName.isEmpty() ? Long.toString(previous.getLongVersionCode()) : previous.versionName;
                if (!containsText(current[0].getWindow().getDecorView(),oldVersion + " → ")) throw new AssertionError("Update omitted the installed version");
            }
            File metadata = new File("/data/system/stone-package-installer/session-" + id, "session.properties");
            if (interrupt && !metadata.setReadOnly()) throw new AssertionError("session completion failure fixture");
            try {
                if (!current[0].findViewById(android.R.id.button1).performClick()) throw new AssertionError("install confirmation button did not click");
                await(() -> results.size() == start + 2 && current[0] == launcher);
            } finally { if (interrupt && !metadata.setWritable(true)) throw new AssertionError("session completion failure fixture cleanup"); }
            int expected = variant.equals("other-bad-signer") ? PackageInstaller.STATUS_FAILURE_CONFLICT : PackageInstaller.STATUS_SUCCESS;
            Intent terminal = results.get(start + 1);
            if (terminal.getIntExtra(PackageInstaller.EXTRA_STATUS, -99) != expected
                    || !PACKAGE.equals(terminal.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME)) || installer.getSessionInfo(id) != null)
                throw new AssertionError("final install status: " + terminal.getExtras());
            PackageInfo info = context.getPackageManager().getPackageInfo(PACKAGE, 0);
            if (interrupt) {
                Properties marker = new Properties();
                marker.setProperty("id", Integer.toString(id)); marker.setProperty("source", info.applicationInfo.sourceDir);
                try (FileOutputStream output = new FileOutputStream(new File(context.getFilesDir(), "commit-recovery.properties"))) {
                    marker.store(output, "Interrupted session completion"); output.getFD().sync();
                }
            }
            return info;
        }
    }

    static void show(Context context, Intent result, Activity[] current) throws Exception {
        if (result.getIntExtra(PackageInstaller.EXTRA_STATUS, -99) != PackageInstaller.STATUS_PENDING_USER_ACTION)
            throw new AssertionError("missing pending-user-action status: " + result.getExtras());
        Intent confirmation = result.getParcelableExtra(Intent.EXTRA_INTENT);
        if (confirmation == null) throw new AssertionError("missing install confirmation intent");
        context.startActivity(new Intent(confirmation).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        await(() -> current[0] != null && current[0].getClass().getName().equals("com.trebledot.stone.runtime.ArtInstallActivity"));
        snapshot(current[0], result.getBooleanExtra(PackageInstaller.EXTRA_PRE_APPROVAL, false)
            ? "/data/art-preapproval-confirmation.png" : "/data/art-install-confirmation.png");
    }

    static void snapshot(Activity activity, String path) throws Exception {
        android.view.View view = activity.getWindow().getDecorView();
        view.measure(android.view.View.MeasureSpec.makeMeasureSpec(360, android.view.View.MeasureSpec.EXACTLY),
            android.view.View.MeasureSpec.makeMeasureSpec(640, android.view.View.MeasureSpec.EXACTLY));
        view.layout(0, 0, 360, 640);
        android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(360, 640, android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas canvas = new android.graphics.Canvas(bitmap);
        view.draw(canvas); canvas.setBitmap(null);
        try (FileOutputStream output = new FileOutputStream(path)) {
            if (!bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, output)) throw new AssertionError("install confirmation PNG");
        } finally { bitmap.recycle(); }
    }
    private static boolean containsText(android.view.View view, String value) {
        if (view instanceof android.widget.TextView && ((android.widget.TextView)view).getText().toString().contains(value)) return true;
        if (view instanceof android.view.ViewGroup) for (int i = 0; i < ((android.view.ViewGroup)view).getChildCount(); i++)
            if (containsText(((android.view.ViewGroup)view).getChildAt(i),value)) return true;
        return false;
    }

    private static int stage(Context context, String root, String variant) throws Exception {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(PACKAGE);
        params.setAppLabel("Untrusted caller label");
        File apk = new File(root, variant + ".apk");
        params.setSize(apk.length());
        int id = installer.createSession(params);
        write(context, id, apk);
        return id;
    }

    static void write(Context context, int id, File apk) throws Exception {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        try (PackageInstaller.Session session = installer.openSession(id); FileInputStream input = new FileInputStream(apk);
                OutputStream output = session.openWrite("base.apk", 0, apk.length())) {
            byte[] buffer = new byte[8192]; int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            session.fsync(output);
        }
    }
}
