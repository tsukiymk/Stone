package com.trebledot.stone.probe;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.res.AssetManager;
import android.os.Bundle;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.io.File;
import java.io.FileInputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Properties;

/** Public PackageInstaller transactions with signed, genuinely distinct installer manifests. */
public final class AutomaticUpdateSample {
    private static final String TARGET = "com.trebledot.stone.install.automatic";
    private static final int PENDING = PackageInstaller.STATUS_PENDING_USER_ACTION;
    private static final int SUCCESS = PackageInstaller.STATUS_SUCCESS;
    private static int sequence = 8200;
    private static final Activity[] current = new Activity[1];
    private static int confirmations;
    private static boolean requestOwnership;
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static int stage(Context context, File file, String name, int action, boolean force) throws Exception {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(name); params.setRequireUserAction(action); params.setSize(file.length());
        params.setRequestUpdateOwnership(requestOwnership);
        if (force) {
            java.lang.reflect.Field flags = params.getClass().getField("installFlags");
            flags.setInt(params,flags.getInt(params) | 0x400);
        }
        int id = installer.createSession(params); CommitProbe.write(context,id,file); return id;
    }
    private static void transaction(Context context, File file, String name, int action, boolean force, int expected,
            boolean approve, boolean nativeConfirmation) throws Exception {
        int created = confirmations;
        try (CommitProbe.Status status = new CommitProbe.Status(context,++sequence,false)) {
            int id = stage(context,file,name,action,force);
            PackageInstaller installer = context.getPackageManager().getPackageInstaller();
            try (PackageInstaller.Session session = installer.openSession(id)) { session.commit(status.pending.getIntentSender()); }
            CommitProbe.await(() -> !status.received.isEmpty());
            Intent first = status.received.get(0);
            check(first.getIntExtra(PackageInstaller.EXTRA_STATUS,-99) == expected,"Unexpected first status for " + file + ": " + first.getExtras());
            if (expected == PENDING) {
                current[0] = null;
                if (nativeConfirmation) {
                    Intent confirmation = first.getParcelableExtra(Intent.EXTRA_INTENT);
                    check(confirmation != null,"Missing native confirmation intent");
                    context.startActivity(new Intent(confirmation).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                } else {
                    CommitProbe.show(context,first,current);
                    check(current[0].findViewById(approve ? android.R.id.button1 : android.R.id.home).performClick(),"Confirmation control failed");
                }
                CommitProbe.await(() -> status.received.size() == 2);
                check(status.received.get(1).getIntExtra(PackageInstaller.EXTRA_STATUS,-99)
                    == (approve ? SUCCESS : PackageInstaller.STATUS_FAILURE_ABORTED),"Unexpected confirmation result");
            } else {
                check(status.received.size() == 1 && first.getParcelableExtra(Intent.EXTRA_INTENT) == null
                    && confirmations == created,"Silent result created a confirmation or emitted pending action");
            }
            check(installer.getSessionInfo(id) == null,"Terminal session remained live");
        }
    }
    private static Object store;
    private static Object call(String name, Class<?>[] types, Object... args) throws Exception {
        java.util.concurrent.FutureTask<Object> task = new java.util.concurrent.FutureTask<>(() -> {
            Method method = store.getClass().getDeclaredMethod(name,types); method.setAccessible(true);
            try { return method.invoke(store,args); }
            catch (InvocationTargetException error) {
                if (error.getCause() instanceof Exception) throw (Exception) error.getCause();
                throw new AssertionError(error.getCause());
            }
        });
        Thread worker = new Thread(task,"Stone automatic-update fixture setup"); worker.setDaemon(true); worker.start();
        CommitProbe.await(task::isDone);
        try { return task.get(); }
        catch (java.util.concurrent.ExecutionException error) {
            if (error.getCause() instanceof Exception) throw (Exception) error.getCause();
            throw new AssertionError(error.getCause());
        }
    }
    private static void install(Context context, File file, String owner) throws Exception {
        call("install",new Class<?>[]{Context.class,String.class,String.class,String.class},context,file.getPath(),owner,TARGET);
    }
    private static void suite(ArtApplication running, boolean allowed) throws Exception {
        Context context = running.context;
        boolean declared = java.util.Arrays.asList(running.apk.packageInfo.requestedPermissions)
            .contains("android.permission.UPDATE_PACKAGES_WITHOUT_USER_ACTION");
        check(declared == allowed,"The actual installer manifest does not match the requested permission scenario");
        try { context.getPackageManager().getPackageInfo(TARGET,0); throw new AssertionError("Use a fresh test profile or clean the previous target"); }
        catch (android.content.pm.PackageManager.NameNotFoundException expected) { }
        java.lang.reflect.Constructor<?> constructor = Class.forName("com.trebledot.stone.runtime.ArtPackageStore").getDeclaredConstructor(ArtPackage.class);
        constructor.setAccessible(true); store = constructor.newInstance(running.apk);
        File root = new File("/data/fixtures"), base = new File(root,"automatic-base.apk"), update = new File(root,"automatic-update.apk");
        if (!allowed) {
            PackageInstaller.SessionParams ownership = new PackageInstaller.SessionParams(1); ownership.setRequestUpdateOwnership(true);
            try { context.getPackageManager().getPackageInstaller().createSession(ownership); throw new AssertionError("Undeclared ownership permission accepted"); }
            catch (SecurityException expected) { }
        }
        transaction(context,base,TARGET,2,false,PENDING,true,false); // A first install must still ask.
        check(java.util.Objects.equals(context.getPackageManager().getInstallSourceInfo(TARGET).getUpdateOwnerPackageName(),
            allowed ? context.getPackageName() : null),"Initial update owner differs from the declared request");
        if (allowed) {
            call("remove",new Class<?>[]{String.class},context.getPackageName());
            android.content.pm.InstallSourceInfo orphan = context.getPackageManager().getInstallSourceInfo(TARGET);
            check(orphan.getInstallingPackageName() == null && orphan.getUpdateOwnerPackageName() == null,
                "Installer removal retained its installation/update authority");
            install(context,base,context.getPackageName());
        }
        PackageInfo before = context.getPackageManager().getPackageInfo(TARGET,0);
        File sentinel = new File(before.applicationInfo.dataDir,"files/automatic-update.data");
        check(sentinel.getParentFile().isDirectory() || sentinel.getParentFile().mkdirs(),"Private directory setup");
        byte[] bytes = new byte[]{19,37,0,82,-1}; java.nio.file.Files.write(sentinel.toPath(),bytes);
        transaction(context,update,TARGET,0,false,PENDING,false,false);
        transaction(context,update,TARGET,1,false,PENDING,false,false);
        transaction(context,update,TARGET,2,true,PENDING,false,false);
        transaction(context,new File(root,"automatic-old-target.apk"),TARGET,2,false,PENDING,false,false);
        install(context,base,"com.trebledot.stone.foreign");
        check(context.getPackageManager().getInstallSourceInfo(TARGET).getUpdateOwnerPackageName() == null,"Foreign confirmed install retained previous update ownership");
        transaction(context,update,TARGET,2,false,PENDING,false,false);
        install(context,base,context.getPackageName());
        transaction(context,new File(root,"automatic-bad-signer.apk"),TARGET,2,false,PackageInstaller.STATUS_FAILURE_CONFLICT,false,false);
        transaction(context,new File(root,"automatic-downgrade.apk"),TARGET,2,false,PackageInstaller.STATUS_FAILURE_CONFLICT,false,false);
        transaction(context,update,TARGET,2,false,allowed ? SUCCESS : PENDING,false,false);
        PackageInfo after = context.getPackageManager().getPackageInfo(TARGET,0);
        check(context.getPackageManager().getInstallSourceInfo(TARGET).getUpdateOwnerPackageName() == null,"Update newly acquired ownership after initial install");
        check(after.getLongVersionCode() == (allowed ? 8 : 7) && after.applicationInfo.uid == before.applicationInfo.uid
            && after.firstInstallTime == before.firstInstallTime && java.util.Arrays.equals(bytes,java.nio.file.Files.readAllBytes(sentinel.toPath())),"Automatic update damaged version, identity or private bytes");
        if (allowed) {
            Object candidate = call("prepare",new Class<?>[]{Context.class,String.class,String.class},context,update.getPath(),TARGET);
            try {
                install(context,update,"com.trebledot.stone.foreign");
                String source = context.getPackageManager().getPackageInfo(TARGET,0).applicationInfo.sourceDir;
                try {
                    call("commit",new Class<?>[]{candidate.getClass(),String.class,String.class,PackageInfo.class},candidate,context.getPackageName(),"automatic-race",running.apk.packageInfo);
                    throw new AssertionError("Changed installer ownership bypassed confirmation at publication");
                } catch (java.io.IOException expected) { check(expected.getClass().getSimpleName().equals("UserActionRequired"),"Wrong race result"); }
                check(source.equals(context.getPackageManager().getPackageInfo(TARGET,0).applicationInfo.sourceDir)
                    && call("receipt",new Class<?>[]{String.class},"automatic-race") == null,"Rejected automatic commit published code or receipt");
            } finally { ((AutoCloseable) candidate).close(); }
            install(context,update,context.getPackageName());
            transaction(context,new File(root,"automatic-installer-update.apk"),context.getPackageName(),2,false,SUCCESS,false,false);
        }
        System.out.println("PASS automatic-update policy: manifest permission=" + allowed
            + "; first install, unspecified/required/forced confirmation, low target, foreign installer, signature, downgrade, private data"
            + (allowed ? ", publication-time ownership race and self update" : ", undeclared permission rejected") + ".");
    }
    private static void baseline(Context context) throws Exception {
        Properties config = new Properties();
        try (FileInputStream input = new FileInputStream("/data/automatic-updates.properties")) { config.load(input); }
        String name = config.getProperty("package"); long version = Long.parseLong(config.getProperty("baseline"));
        transaction(context,new File("/data/automatic-baseline.apk"),name,1,false,PENDING,true,true);
        PackageInfo installed = context.getPackageManager().getPackageInfo(name,0);
        check(installed.getLongVersionCode() == version && context.getPackageName().equals(
            context.getPackageManager().getInstallSourceInfo(name).getInstallingPackageName()),"Baseline version or actual installer differs");
        File sentinel = new File(installed.applicationInfo.dataDir,"files/automatic-update.data");
        check(sentinel.getParentFile().isDirectory() || sentinel.getParentFile().mkdirs(),"Baseline private directory");
        java.nio.file.Files.write(sentinel.toPath(),new byte[]{19,37,0,82,-1});
        System.out.println("PASS authentic baseline through public PackageInstaller, native confirmation and " + context.getPackageName() + ": " + name + " version=" + version);
    }
    public static int run(AssetManager assets, String scenario) throws Exception {
        ArtApplication running = ArtApplication.start(ArtPackage.openApplication(assets,"/data/application.apk"));
        requestOwnership = java.util.Arrays.asList(running.apk.packageInfo.requestedPermissions).contains("android.permission.ENFORCE_UPDATE_OWNERSHIP");
        Application.ActivityLifecycleCallbacks lifecycle = new Application.ActivityLifecycleCallbacks() {
            public void onActivityCreated(Activity activity,Bundle state) { confirmations++; }
            public void onActivityStarted(Activity activity) { }
            public void onActivityResumed(Activity activity) { current[0] = activity; }
            public void onActivityPaused(Activity activity) { }
            public void onActivityStopped(Activity activity) { }
            public void onActivitySaveInstanceState(Activity activity,Bundle state) { }
            public void onActivityDestroyed(Activity activity) { }
        };
        running.application.registerActivityLifecycleCallbacks(lifecycle);
        try {
            if (scenario.equals("baseline")) baseline(running.context);
            else if (scenario.equals("allowed") || scenario.equals("denied")) suite(running,scenario.equals("allowed"));
            else throw new IllegalArgumentException("Unknown automatic-update scenario");
        } finally { running.destroyActivity(); running.application.unregisterActivityLifecycleCallbacks(lifecycle); }
        return 42;
    }
}
