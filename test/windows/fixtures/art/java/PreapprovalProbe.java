package com.trebledot.stone.probe;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.icu.util.ULocale;
import android.os.Bundle;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;

/** Public preapproval followed by verified installation, including durable approval recovery. */
final class PreapprovalProbe {
    private static final String PACKAGE = "com.trebledot.stone.install.preapproved";
    private static final String LABEL = "Application préapprouvée";
    private static PackageInstaller.PreapprovalDetails details(String name, CharSequence label) {
        return new PackageInstaller.PreapprovalDetails.Builder().setPackageName(name).setLabel(label).setLocale(ULocale.FRANCE).build();
    }
    private static void check(boolean condition, String message) { if (!condition) throw new AssertionError(message); }
    private static void status(Intent intent, int expected, boolean preapproval) {
        check(intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -99) == expected
            && intent.getBooleanExtra(PackageInstaller.EXTRA_PRE_APPROVAL, !preapproval) == preapproval,
            "preapproval/install status: " + intent.getExtras());
    }
    private static int create(Context context, String name) throws Exception {
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        if (name != null) params.setAppPackageName(name);
        return context.getPackageManager().getPackageInstaller().createSession(params);
    }
    private static void decide(Context context, CommitProbe.Status result, Activity[] current, boolean approve) throws Exception {
        CommitProbe.await(() -> !result.received.isEmpty());
        status(result.received.get(0), PackageInstaller.STATUS_PENDING_USER_ACTION, true);
        CommitProbe.show(context, result.received.get(0), current);
        check(current[0].findViewById(approve ? android.R.id.button1 : android.R.id.home).performClick(), "preapproval button");
        CommitProbe.await(() -> result.received.size() == 2);
        status(result.received.get(1), approve ? PackageInstaller.STATUS_SUCCESS : PackageInstaller.STATUS_FAILURE_ABORTED, true);
    }
    private static void approve(Context context, int id, String name, String label, int code, Activity[] current) throws Exception {
        try (CommitProbe.Status result = new CommitProbe.Status(context, code, false);
                PackageInstaller.Session session = context.getPackageManager().getPackageInstaller().openSession(id)) {
            session.requestUserPreapproval(details(name, label), result.pending.getIntentSender());
            decide(context, result, current, true);
        }
    }
    private static void commit(Context context, int id, int expected, int code) throws Exception {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        try (CommitProbe.Status result = new CommitProbe.Status(context, code, false); PackageInstaller.Session session = installer.openSession(id)) {
            session.commit(result.pending.getIntentSender());
            CommitProbe.await(() -> !result.received.isEmpty());
            check(result.received.size() == 1, "preapproved commit requested a second confirmation");
            status(result.received.get(0), expected, false);
            check(installer.getSessionInfo(id) == null, "completed preapproved session is still visible");
        }
    }
    static void recover(Context context) throws Exception {
        if (System.getProperty("stone.probe.archives") == null) return;
        File marker = new File(context.getFilesDir(), "preapproval-recovery.properties");
        if (!marker.isFile()) return;
        Properties state = new Properties();
        try (FileInputStream input = new FileInputStream(marker)) { state.load(input); }
        int id = Integer.parseInt(state.getProperty("id"));
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        check(installer.getSessionInfo(id) != null && installer.getSessionInfo(id).isPreApprovalRequested()
            && !installer.getSessionInfo(id).isCommitted(), "approved writable session did not recover");
        int uid = context.getPackageManager().getPackageInfo(PACKAGE, 0).applicationInfo.uid;
        commit(context, id, PackageInstaller.STATUS_SUCCESS, 5200);
        PackageInfo restored = context.getPackageManager().getPackageInfo(PACKAGE, 0);
        check(restored.getLongVersionCode() == 8 && restored.applicationInfo.uid == uid, "preapproved upgrade recovery lost package identity");
        check(marker.delete(), "preapproval recovery marker cleanup");
        System.out.println("ART recovered persisted preapproval and staged APK bytes, then verified and committed an upgrade without a second confirmation.");
    }
    static void run(ArtApplication running) throws Exception {
        String fixtures = System.getProperty("stone.probe.archives");
        if (fixtures == null) return;
        Context context = running.context;
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        Class<?> storeType = Class.forName("com.trebledot.stone.runtime.ArtPackageStore");
        java.lang.reflect.Constructor<?> constructor = storeType.getDeclaredConstructor(ArtPackage.class); constructor.setAccessible(true);
        java.lang.reflect.Method remove = storeType.getDeclaredMethod("remove", String.class); remove.setAccessible(true);
        remove.invoke(constructor.newInstance(running.apk), PACKAGE);
        List<Integer> finished = new ArrayList<>();
        PackageInstaller.SessionCallback sessions = new PackageInstaller.SessionCallback() {
            public void onCreated(int id) { }
            public void onBadgingChanged(int id) { }
            public void onActiveChanged(int id, boolean active) { }
            public void onProgressChanged(int id, float progress) { }
            public void onFinished(int id, boolean success) { finished.add(id); }
        };
        installer.registerSessionCallback(sessions);
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
        File source = new File(fixtures, "preapproval.apk");
        try {
            int restricted = create(context, PACKAGE);
            PendingIntent immutable = PendingIntent.getBroadcast(context, 5290,
                new Intent("com.trebledot.stone.IMMUTABLE_PREAPPROVAL").setPackage(context.getPackageName()), PendingIntent.FLAG_IMMUTABLE);
            int target = running.apk.applicationInfo.targetSdkVersion;
            try (PackageInstaller.Session session = installer.openSession(restricted);
                    CommitProbe.Status result = new CommitProbe.Status(context, 5291, false)) {
                running.apk.applicationInfo.targetSdkVersion = 35;
                try { session.requestUserPreapproval(details(PACKAGE, LABEL), immutable.getIntentSender()); throw new AssertionError("immutable preapproval status accepted for SDK 35"); }
                catch (IllegalArgumentException expected) { }
                check(!installer.getSessionInfo(restricted).isPreApprovalRequested(), "rejected receiver changed preapproval state");
                try { session.requestUserPreapproval(details("another.package", LABEL), result.pending.getIntentSender()); throw new AssertionError("preapproval disagreed with session package"); }
                catch (IllegalArgumentException expected) { }
            } finally { running.apk.applicationInfo.targetSdkVersion = target; immutable.cancel(); installer.abandonSession(restricted); }
            // Same PendingIntent receives preapproval and commit results, distinguished by the API extra.
            int id = create(context, PACKAGE);
            try (CommitProbe.Status result = new CommitProbe.Status(context, 5201, false); PackageInstaller.Session session = installer.openSession(id)) {
                StringBuilder callerLabel = new StringBuilder(LABEL);
                session.requestUserPreapproval(details(PACKAGE, callerLabel), result.pending.getIntentSender());
                callerLabel.append(" changed by caller");
                check(installer.getSessionInfo(id).isPreApprovalRequested() && !installer.getSessionInfo(id).isCommitted(), "preapproval sealed session");
                try { session.requestUserPreapproval(details(PACKAGE, LABEL), result.pending.getIntentSender()); throw new AssertionError("duplicate preapproval accepted"); }
                catch (IllegalStateException expected) { }
                decide(context, result, current, true);
                CommitProbe.await(() -> current[0] == launcher);
                check(!finished.contains(id) && installer.getSessionInfo(id) != null, "preapproval success finished installation");
                String locale = java.util.Locale.getDefault().toLanguageTag();
                CommitProbe.write(context, id, source);
                session.commit(result.pending.getIntentSender());
                CommitProbe.await(() -> result.received.size() == 3);
                status(result.received.get(2), PackageInstaller.STATUS_SUCCESS, false);
                check(locale.equals(java.util.Locale.getDefault().toLanguageTag()), "localized label validation changed process locale");
                CommitProbe.await(() -> finished.contains(id));
            }
            String installed = context.getPackageManager().getPackageInfo(PACKAGE, 0).applicationInfo.sourceDir;
            PackageInstaller.SessionParams inherit = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_INHERIT_EXISTING);
            inherit.setAppPackageName(PACKAGE);
            int blocked = installer.createSession(inherit);
            try (CommitProbe.Status result = new CommitProbe.Status(context, 5292, false); PackageInstaller.Session session = installer.openSession(blocked)) {
                session.requestUserPreapproval(details(PACKAGE, LABEL), result.pending.getIntentSender());
                CommitProbe.await(() -> result.received.size() == 1);
                status(result.received.get(0), PackageInstaller.STATUS_FAILURE_BLOCKED, true);
                check(installer.getSessionInfo(blocked) != null && !installer.getSessionInfo(blocked).isCommitted(), "unavailable preapproval destroyed or sealed session");
            } finally { installer.abandonSession(blocked); }
            int denied = create(context, PACKAGE);
            try (CommitProbe.Status result = new CommitProbe.Status(context, 5202, false); PackageInstaller.Session session = installer.openSession(denied)) {
                session.requestUserPreapproval(details(PACKAGE, LABEL), result.pending.getIntentSender());
                decide(context, result, current, false);
                check(installer.getSessionInfo(denied) == null, "declined preapproval did not abandon session");
            }
            int mismatch = create(context, PACKAGE);
            approve(context, mismatch, PACKAGE, LABEL, 5203, current);
            CommitProbe.write(context, mismatch, new File(fixtures, "preapproval-label-changed.apk"));
            commit(context, mismatch, PackageInstaller.STATUS_FAILURE_INVALID, 5204);
            check(installed.equals(context.getPackageManager().getPackageInfo(PACKAGE, 0).applicationInfo.sourceDir), "label mismatch replaced installed APK");
            int different = create(context, null);
            approve(context, different, "com.trebledot.stone.install.another", LABEL, 5205, current);
            CommitProbe.write(context, different, source);
            commit(context, different, PackageInstaller.STATUS_FAILURE_INVALID, 5206);
            int conflict = create(context, PACKAGE);
            approve(context, conflict, PACKAGE, LABEL, 5207, current);
            CommitProbe.write(context, conflict, new File(fixtures, "preapproval-bad-signer.apk"));
            commit(context, conflict, PackageInstaller.STATUS_FAILURE_CONFLICT, 5208);
            int invalid = create(context, "com.trebledot.stone.install.fixture");
            approve(context, invalid, "com.trebledot.stone.install.fixture", "安装签名测试", 5209, current);
            CommitProbe.write(context, invalid, new File(fixtures, "tampered-v2.apk"));
            commit(context, invalid, PackageInstaller.STATUS_FAILURE_INVALID, 5210);

            int unresolved = create(context, PACKAGE);
            try (CommitProbe.Status result = new CommitProbe.Status(context, 5211, false); PackageInstaller.Session session = installer.openSession(unresolved)) {
                session.requestUserPreapproval(details(PACKAGE, LABEL), result.pending.getIntentSender());
                CommitProbe.await(() -> result.received.size() == 1);
                Intent stale = result.received.get(0).getParcelableExtra(Intent.EXTRA_INTENT);
                CommitProbe.write(context, unresolved, source);
                session.commit(result.pending.getIntentSender());
                CommitProbe.await(() -> result.received.size() == 3);
                status(result.received.get(1), PackageInstaller.STATUS_FAILURE_BLOCKED, true);
                status(result.received.get(2), PackageInstaller.STATUS_PENDING_USER_ACTION, false);
                try { context.startActivity(new Intent(stale).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); throw new AssertionError("old preapproval token accepted at commit"); }
                catch (android.content.ActivityNotFoundException expected) { }
                CommitProbe.show(context, result.received.get(2), current);
                current[0].findViewById(android.R.id.home).performClick();
                CommitProbe.await(() -> result.received.size() == 4);
                status(result.received.get(3), PackageInstaller.STATUS_FAILURE_ABORTED, false);
            }
            // A failed approval write must not silently authorize a later commit.
            int failed = create(context, PACKAGE);
            try (CommitProbe.Status result = new CommitProbe.Status(context, 5212, false); PackageInstaller.Session session = installer.openSession(failed)) {
                session.requestUserPreapproval(details(PACKAGE, LABEL), result.pending.getIntentSender());
                CommitProbe.await(() -> result.received.size() == 1);
                CommitProbe.show(context, result.received.get(0), current);
                File metadata = new File("/data/system/stone-package-installer/session-" + failed, "session.properties");
                check(metadata.setReadOnly(), "approval failure fixture");
                try { current[0].findViewById(android.R.id.button1).performClick(); }
                finally { check(metadata.setWritable(true), "approval failure cleanup"); }
                CommitProbe.await(() -> result.received.size() == 2);
                status(result.received.get(1), PackageInstaller.STATUS_FAILURE_STORAGE, true);
                CommitProbe.write(context, failed, source);
                session.commit(result.pending.getIntentSender());
                CommitProbe.await(() -> result.received.size() == 3);
                status(result.received.get(2), PackageInstaller.STATUS_PENDING_USER_ACTION, false);
                installer.abandonSession(failed);
                CommitProbe.await(() -> result.received.size() == 4);
                status(result.received.get(3), PackageInstaller.STATUS_FAILURE_ABORTED, false);
            }
            int recovery = create(context, PACKAGE);
            approve(context, recovery, PACKAGE, LABEL, 5213, current);
            CommitProbe.write(context, recovery, new File(fixtures, "preapproval-update.apk"));
            Properties marker = new Properties(); marker.setProperty("id", Integer.toString(recovery));
            try (FileOutputStream output = new FileOutputStream(new File(context.getFilesDir(), "preapproval-recovery.properties"))) {
                marker.store(output, "Approved install awaiting commit after restart"); output.getFD().sync();
            }
            CommitProbe.await(() -> {
                for (Thread thread : Thread.getAllStackTraces().keySet()) if (thread.getName().startsWith("Stone APK commit ") && thread.isAlive()) return false;
                return true;
            });
        } finally {
            running.destroyActivity();
            running.application.unregisterActivityLifecycleCallbacks(lifecycle);
            installer.unregisterSessionCallback(sessions);
        }
        System.out.println("ART preapproved writable sessions, verified localized labels and signatures at commit, rejected mismatches, cancelled unresolved approval and preserved durable consent.");
    }
}
