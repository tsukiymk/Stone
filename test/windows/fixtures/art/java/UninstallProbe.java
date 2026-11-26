package com.trebledot.stone.probe;

import android.app.Activity;
import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileInputStream;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;

/** Public removal Intent, real Activity results, package broadcasts and owned file cleanup. */
public final class UninstallProbe {
    private static final String PACKAGE = "com.trebledot.stone.install.removal";
    public static final class Caller extends Activity {
        int results, code;
        Intent result;
        @Override protected void onCreate(Bundle saved) { super.onCreate(saved); setContentView(new android.widget.TextView(this)); }
        @Override protected void onActivityResult(int request, int code, Intent data) {
            if (request != 9523) throw new AssertionError("uninstall Activity result routing");
            results++; this.code = code; result = data;
        }
    }
    private static boolean installed(Context context) throws Exception {
        try { context.getPackageManager().getPackageInfo(PACKAGE, 0); return true; }
        catch (PackageManager.NameNotFoundException expected) { return false; }
    }
    private static Activity prompt(Caller caller, Activity[] current) throws Exception {
        caller.startActivityForResult(new Intent(Intent.ACTION_DELETE, Uri.fromParts("package", PACKAGE, null))
            .putExtra(Intent.EXTRA_RETURN_RESULT, true), 9523);
        CommitProbe.await(() -> current[0] != null && current[0].getClass().getName().equals("com.trebledot.stone.runtime.ArtUninstallActivity")
            && current[0].findViewById(android.R.id.button1).isEnabled());
        return current[0];
    }
    static void run(ArtApplication running) throws Exception {
        String root = System.getProperty("stone.probe.archives");
        if (root == null) return;
        Class<?> storeType = Class.forName("com.trebledot.stone.runtime.ArtPackageStore");
        java.lang.reflect.Constructor<?> constructor = storeType.getDeclaredConstructor(ArtPackage.class); constructor.setAccessible(true);
        Object store = constructor.newInstance(running.apk);
        File marker = new File(running.context.getFilesDir(), "uninstall-recovery.properties");
        if (marker.isFile()) {
            Properties previous = new Properties();
            try (FileInputStream input = new FileInputStream(marker)) { previous.load(input); }
            Method pending = storeType.getDeclaredMethod("pendingRemovals"); pending.setAccessible(true);
            CommitProbe.await(() -> !installed(running.context) && !new File(previous.getProperty("source")).exists()
                && !new File(previous.getProperty("data")).exists() && ((List<?>) pending.invoke(store)).isEmpty());
            if (running.context.getPackageManager().getPackagesForUid(Integer.parseInt(previous.getProperty("uid"))) != null
                    || !marker.delete()) throw new AssertionError("recovered uninstall UID or marker");
            System.out.println("ART recovered a committed package removal from the previous process and finished owned-file cleanup.");
        }
        Method install = storeType.getDeclaredMethod("install", Context.class, String.class, String.class, String.class); install.setAccessible(true);
        PackageInfo first = (PackageInfo) install.invoke(store, running.context, root + "/uninstall.apk", running.context.getPackageName(), PACKAGE);
        File notificationPolicy = new File("/data/system/stone-notifications/policy.properties");
        byte[] originalPolicy = notificationPolicy.isFile() ? java.nio.file.Files.readAllBytes(notificationPolicy.toPath()) : null;
        notificationPolicy.getParentFile().mkdirs();
        Properties notificationPermissions = new Properties();
        if (originalPolicy != null) notificationPermissions.load(new java.io.ByteArrayInputStream(originalPolicy));
        notificationPermissions.setProperty("format", "1"); notificationPermissions.setProperty("global", "true");
        notificationPermissions.setProperty("package." + PACKAGE, "true");
        notificationPermissions.setProperty("package.com.trebledot.stone.install.fixture", "false");
        try (FileOutputStream output = new FileOutputStream(notificationPolicy)) { notificationPermissions.store(output, "Notification removal fixture"); }
        File notificationIdentity = new File("/data/system/stone-notifications/apps/" + PACKAGE + ".json"); notificationIdentity.getParentFile().mkdirs();
        String identity = new org.json.JSONObject().put("format", 1).put("apps", new org.json.JSONArray().put(new org.json.JSONObject()
            .put("packageName", PACKAGE).put("name", "Removal test").put("uid", first.applicationInfo.uid))).toString();
        try (FileOutputStream output = new FileOutputStream(notificationIdentity)) { output.write(identity.getBytes(java.nio.charset.StandardCharsets.UTF_8)); }
        File privateFile = new File(first.applicationInfo.dataDir, "files/keep-on-cancel.txt");
        if (!privateFile.getParentFile().isDirectory() && !privateFile.getParentFile().mkdirs()) throw new AssertionError("uninstall test data");
        try (FileOutputStream output = new FileOutputStream(privateFile)) { output.write(42); }
        File outside = new File(running.context.getFilesDir(), "uninstall-outside-sentinel.txt");
        try (FileOutputStream output = new FileOutputStream(outside)) { output.write(73); }
        List<Intent> changes = new ArrayList<>();
        BroadcastReceiver receiver = new BroadcastReceiver() {
            public void onReceive(Context context, Intent intent) {
                if (intent.getData() != null && PACKAGE.equals(intent.getData().getSchemeSpecificPart())
                        && intent.getIntExtra(Intent.EXTRA_UID, -1) == first.applicationInfo.uid
                        && !intent.getBooleanExtra(Intent.EXTRA_REPLACING, false)) changes.add(new Intent(intent));
            }
        };
        IntentFilter filter = new IntentFilter(Intent.ACTION_PACKAGE_REMOVED);
        filter.addAction(Intent.ACTION_PACKAGE_FULLY_REMOVED); filter.addDataScheme("package");
        running.context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        Activity[] current = new Activity[1];
        Application.ActivityLifecycleCallbacks lifecycle = new Application.ActivityLifecycleCallbacks() {
            public void onActivityCreated(Activity activity, Bundle state) { }
            public void onActivityStarted(Activity activity) { }
            public void onActivityResumed(Activity activity) { current[0] = activity; }
            public void onActivityPaused(Activity activity) { }
            public void onActivityStopped(Activity activity) { }
            public void onActivitySaveInstanceState(Activity activity, Bundle state) { }
            public void onActivityDestroyed(Activity activity) { }
        };
        running.application.registerActivityLifecycleCallbacks(lifecycle);
        String original = running.apk.activityInfo.name;
        running.apk.activityInfo.name = Caller.class.getName();
        try {
            Caller caller = (Caller) running.startActivity();
            String[] permissions = running.apk.packageInfo.requestedPermissions;
            try {
                running.apk.packageInfo.requestedPermissions = java.util.Arrays.stream(permissions)
                    .filter(value -> !value.equals("android.permission.REQUEST_DELETE_PACKAGES")).toArray(String[]::new);
                caller.startActivityForResult(new Intent(Intent.ACTION_DELETE, Uri.fromParts("package", PACKAGE, null)), 9523);
                CommitProbe.await(() -> current[0] != null && current[0].getClass().getName().equals("com.trebledot.stone.runtime.ArtUninstallActivity"));
                if (current[0].findViewById(android.R.id.button1).isEnabled()) throw new AssertionError("Undeclared delete permission accepted");
                current[0].findViewById(android.R.id.home).performClick();
                CommitProbe.await(() -> caller.results == 1);
                if (!installed(running.context) || !privateFile.isFile()) throw new AssertionError("Rejected uninstall changed data");
                caller.results = 0;
            } finally { running.apk.packageInfo.requestedPermissions = permissions; }
            Activity cancelled = prompt(caller, current);
            cancelled.findViewById(android.R.id.home).performClick();
            CommitProbe.await(() -> caller.results == 1);
            if (caller.code != Activity.RESULT_CANCELED || !installed(running.context) || !privateFile.isFile() || !changes.isEmpty())
                throw new AssertionError("cancelled uninstall changed package or data");
            Properties retained = new Properties(); try (FileInputStream input = new FileInputStream(notificationPolicy)) { retained.load(input); }
            if (!"true".equals(retained.getProperty("package." + PACKAGE)) || !notificationIdentity.isFile())
                throw new AssertionError("Cancelled uninstall revoked notification consent");
            Activity stale = prompt(caller, current);
            PackageInfo next = (PackageInfo) install.invoke(store, running.context, root + "/uninstall-update.apk", running.context.getPackageName(), PACKAGE);
            stale.findViewById(android.R.id.button1).performClick();
            CommitProbe.await(() -> stale.findViewById(android.R.id.home).isEnabled());
            if (!installed(running.context) || !new File(next.applicationInfo.sourceDir).isFile() || !privateFile.isFile() || !changes.isEmpty())
                throw new AssertionError("stale uninstall confirmation removed a newer package");
            stale.findViewById(android.R.id.home).performClick();
            CommitProbe.await(() -> caller.results == 2);
            Activity removal = prompt(caller, current);
            removal.recreate();
            CommitProbe.await(() -> current[0] != removal && current[0].getClass().getName().equals("com.trebledot.stone.runtime.ArtUninstallActivity")
                && current[0].findViewById(android.R.id.button1).isEnabled());
            Activity recreated = current[0];
            CommitProbe.snapshot(recreated, "/data/art-uninstall-confirmation.png");
            File blocked = new File("/data/system/stone-packages/packages.properties.tmp");
            if (!blocked.mkdir()) throw new AssertionError("uninstall registry failure injection");
            recreated.findViewById(android.R.id.button1).performClick();
            CommitProbe.await(() -> recreated.findViewById(android.R.id.home).isEnabled());
            if (!installed(running.context) || !privateFile.isFile() || !changes.isEmpty() || caller.results != 2)
                throw new AssertionError("failed removal publication deleted data or returned success");
            if (blocked.exists() && !blocked.delete()) throw new AssertionError("registry failure injection cleanup");
            recreated.findViewById(android.R.id.button1).performClick();
            CommitProbe.await(() -> caller.results == 3 && changes.size() == 2);
            if (caller.code != Activity.RESULT_OK || caller.result.getIntExtra("android.intent.extra.INSTALL_RESULT", -99) != 1
                    || installed(running.context) || new File(first.applicationInfo.sourceDir).exists()
                    || new File(next.applicationInfo.sourceDir).exists() || new File(next.applicationInfo.dataDir).exists()
                    || !outside.isFile() || running.context.getPackageManager().getPackagesForUid(next.applicationInfo.uid) != null)
                throw new AssertionError("uninstall result, query, UID or owned file cleanup");
            Properties revoked = new Properties(); try (FileInputStream input = new FileInputStream(notificationPolicy)) { revoked.load(input); }
            if (revoked.containsKey("package." + PACKAGE) || notificationIdentity.exists()
                    || !"true".equals(revoked.getProperty("global")) || !"false".equals(revoked.getProperty("package.com.trebledot.stone.install.fixture")))
                throw new AssertionError("Uninstall did not revoke only the removed package's notification consent");
            System.out.println("ART notification consent and observed identity were retained on cancellation and revoked on completed uninstall.");
            for (Intent change : changes) if (!change.getBooleanExtra(Intent.EXTRA_DATA_REMOVED, false)
                    || change.getBooleanExtra(Intent.EXTRA_REPLACING, true) || change.getIntExtra(Intent.EXTRA_UID, -1) != next.applicationInfo.uid)
                throw new AssertionError("uninstall broadcast payload");
            for (PackageInfo info : running.context.getPackageManager().getInstalledPackages(0))
                if (PACKAGE.equals(info.packageName)) throw new AssertionError("uninstalled package stayed in list");
            System.out.println("ART uninstall result and cleanup passed; preparing interrupted removal recovery.");
            PackageInfo recovery = (PackageInfo) install.invoke(store, running.context, root + "/uninstall.apk", running.context.getPackageName(), PACKAGE);
            if (recovery.applicationInfo.uid <= next.applicationInfo.uid) throw new AssertionError("Reinstall reused removed UID");
            File recoveryData = new File(recovery.applicationInfo.dataDir, "pending-cleanup.txt");
            try (FileOutputStream output = new FileOutputStream(recoveryData)) { output.write(91); }
            Method mark = storeType.getDeclaredMethod("markRemoval", String.class, String.class, List.class); mark.setAccessible(true);
            mark.invoke(store, PACKAGE, recovery.applicationInfo.sourceDir, new ArrayList<String>());
            System.out.println("ART persisted interrupted removal; checking installation is blocked until cleanup.");
            if (installed(running.context) || !recoveryData.isFile() || !new File(recovery.applicationInfo.sourceDir).isFile())
                throw new AssertionError("Durable uninstall phase did not preserve pending cleanup state");
            try {
                install.invoke(store, running.context, root + "/uninstall-update.apk", running.context.getPackageName(), PACKAGE);
                throw new AssertionError("Installation was accepted during pending removal");
            } catch (java.lang.reflect.InvocationTargetException expected) {
                if (!expected.getCause().getMessage().contains("removal")) throw expected;
            }
            Properties recoveryMarker = new Properties();
            recoveryMarker.setProperty("source", recovery.applicationInfo.sourceDir);
            recoveryMarker.setProperty("data", recovery.applicationInfo.dataDir);
            recoveryMarker.setProperty("uid", Integer.toString(recovery.applicationInfo.uid));
            try (FileOutputStream output = new FileOutputStream(marker)) { recoveryMarker.store(output, "Interrupted uninstall cleanup"); output.getFD().sync(); }
        } finally {
            running.destroyActivity(); running.apk.activityInfo.name = original;
            running.application.unregisterActivityLifecycleCallbacks(lifecycle);
            running.context.unregisterReceiver(receiver); outside.delete();
            if (originalPolicy == null) { if (notificationPolicy.exists() && !notificationPolicy.delete()) throw new AssertionError("notification policy fixture cleanup"); }
            else java.nio.file.Files.write(notificationPolicy.toPath(), originalPolicy);
        }
        System.out.println("ART cancelled and confirmed package removal, retained confirmation across recreation, rejected stale identity, rolled back failed registry publication, removed owned code/data and delivered Activity results and package broadcasts.");
    }
}
