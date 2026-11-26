package com.trebledot.stone.probe;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.util.List;
import java.util.Properties;

/** Installed launch queries and migration, through the image's public PackageManager. */
final class LauncherProbe {
    private static final String PACKAGE = "com.trebledot.stone.install.launcher";
    private static Object store;
    private static Class<?> type;
    private static ArtApplication application;

    private static Object call(String name, Class<?>[] parameters, Object... arguments) throws Exception {
        Method method = type.getDeclaredMethod(name, parameters); method.setAccessible(true);
        try { return method.invoke(store, arguments); }
        catch (java.lang.reflect.InvocationTargetException error) { throw new IllegalStateException(name, error.getCause()); }
    }

    private static PackageInfo install(String variant) throws Exception {
        return (PackageInfo) call("install", new Class<?>[]{Context.class, String.class, String.class, String.class},
            application.context, System.getProperty("stone.probe.archives") + "/" + variant + ".apk",
            application.context.getPackageName(), PACKAGE);
    }

    private static void enabled(PackageInfo info, String component) throws Exception {
        File file = new File(info.applicationInfo.dataDir, ".stone-components.properties");
        if (component == null) { if (file.exists() && !file.delete()) throw new AssertionError("component state cleanup"); return; }
        Properties states = new Properties(); states.setProperty(component, "2");
        try (FileOutputStream output = new FileOutputStream(file)) { states.store(output, "Disabled test component"); }
    }

    private static Intent expect(String name) throws Exception {
        PackageManager manager = application.context.getPackageManager();
        Intent launch = manager.getLaunchIntentForPackage(PACKAGE);
        if (launch == null || !new ComponentName(PACKAGE, PACKAGE + name).equals(launch.getComponent())
                || !Intent.ACTION_MAIN.equals(launch.getAction()) || !launch.hasCategory(Intent.CATEGORY_LAUNCHER)
                || (launch.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) == 0)
            throw new AssertionError("Installed launcher intent: " + launch);
        if (!manager.getActivityInfo(launch.getComponent(), 0).exported || manager.resolveActivity(launch, 0) == null)
            throw new AssertionError("Installed explicit launcher metadata");
        return launch;
    }

    static void run(ArtApplication running) throws Exception {
        application = running;
        type = Class.forName("com.trebledot.stone.runtime.ArtPackageStore");
        java.lang.reflect.Constructor<?> constructor = type.getDeclaredConstructor(ArtPackage.class); constructor.setAccessible(true);
        store = constructor.newInstance(running.apk);
        call("remove", new Class<?>[]{String.class}, PACKAGE);
        PackageInfo first = install("launcher");
        enabled(first, null);
        Intent launch = expect(".Start");
        PackageManager manager = running.context.getPackageManager();
        List<ResolveInfo> matches = manager.queryIntentActivities(new Intent(Intent.ACTION_MAIN).setPackage(PACKAGE)
            .addCategory(Intent.CATEGORY_LAUNCHER), PackageManager.MATCH_DEFAULT_ONLY);
        if (matches.size() != 1 || !matches.get(0).activityInfo.name.equals(PACKAGE + ".Start"))
            throw new AssertionError("Default/exported/enabled launcher filtering");
        matches.get(0).activityInfo.name = "mutated"; expect(".Start");
        enabled(first, PACKAGE + ".Start");
        if (manager.getLaunchIntentForPackage(PACKAGE) != null) throw new AssertionError("Disabled alias resolves");
        if (manager.getActivityInfo(launch.getComponent(), PackageManager.MATCH_DISABLED_COMPONENTS) == null)
            throw new AssertionError("Disabled component metadata is unavailable");
        enabled(first, "com.trebledot.stone.probe.LauncherFixtureActivity");
        if (manager.getLaunchIntentForPackage(PACKAGE) != null) throw new AssertionError("Alias resolves with disabled target");
        enabled(first, null); expect(".Start");
        File index = new File("/data/system/stone-packages/packages.properties");
        Properties legacy = new Properties();
        try (FileInputStream input = new FileInputStream(index)) { legacy.load(input); }
        legacy.remove("launchers." + PACKAGE);
        File pending = new File(index.getParentFile(), "launcher-migration.properties");
        try (FileOutputStream output = new FileOutputStream(pending)) { legacy.store(output, "Legacy launcher index"); output.getFD().sync(); }
        android.system.Os.rename(pending.getPath(), index.getPath());
        call("initializeLaunchers", new Class<?>[0]); expect(".Start");
        PackageInfo updated = install("launcher-update"); expect(".Updated");
        if (updated.applicationInfo.uid != first.applicationInfo.uid) throw new AssertionError("Launcher upgrade changed UID");
        install("launcher-service");
        if (manager.getLaunchIntentForPackage(PACKAGE) != null || manager.resolveActivity(launch, 0) != null)
            throw new AssertionError("Service-only update retained a stale launcher");
        call("remove", new Class<?>[]{String.class}, PACKAGE);
        PackageInfo disabled = install("launcher-disabled");
        if (manager.getLaunchIntentForPackage(PACKAGE) != null) throw new AssertionError("Disabled application resolves");
        call("remove", new Class<?>[]{String.class}, PACKAGE);
        PackageInfo seeded = install("launcher"); expect(".Start");
        File sentinel = new File(seeded.applicationInfo.dataDir, "files/installed-sentinel.txt");
        if (!sentinel.getParentFile().isDirectory() && !sentinel.getParentFile().mkdirs()) throw new AssertionError("Seed data directory");
        try (FileOutputStream output = new FileOutputStream(sentinel)) { output.write(42); }
        Properties expected = new Properties();
        expected.setProperty("uid", Integer.toString(seeded.applicationInfo.uid));
        expected.setProperty("source", seeded.applicationInfo.sourceDir);
        expected.setProperty("data", seeded.applicationInfo.dataDir);
        expected.setProperty("staleSource", first.applicationInfo.sourceDir);
        try (FileOutputStream output = new FileOutputStream("/data/installed-launch-expected.properties")) {
            expected.store(output, "Managed APK for native launch verification"); output.getFD().sync();
        }
        CommitProbe.await(() -> {
            File catalogFile = new File("/data/system/stone-packages/packages.json");
            if (!catalogFile.isFile()) return false;
            org.json.JSONObject catalog = new org.json.JSONObject(new String(java.nio.file.Files.readAllBytes(catalogFile.toPath()),
                java.nio.charset.StandardCharsets.UTF_8));
            if (catalog.getInt("format") != 1) throw new AssertionError("settings catalog format");
            org.json.JSONArray apps = catalog.getJSONArray("apps");
            for (int position = 0; position < apps.length(); position++) {
                org.json.JSONObject app = apps.getJSONObject(position);
                if (PACKAGE.equals(app.getString("packageName")) && app.getInt("uid") == seeded.applicationInfo.uid)
                    return app.getString("name").equals("安装签名测试")
                        && app.getLong("codeBytes") == new File(seeded.applicationInfo.sourceDir).length();
            }
            return false;
        });
        System.out.println("ART installed launcher queries, alias/target enablement, migration, version replacement and service-only APK checks passed.");
    }
}
