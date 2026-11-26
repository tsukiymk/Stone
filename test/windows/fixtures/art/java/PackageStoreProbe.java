package com.trebledot.stone.probe;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Properties;

/** Internal store transactions, observed through the public image PackageManager. */
final class PackageStoreProbe {
    private static final String PACKAGE = "com.trebledot.stone.install.fixture";
    private static final String OTHER = "com.trebledot.stone.install.other";
    private static final int FLAGS = PackageManager.GET_SIGNING_CERTIFICATES | PackageManager.GET_SIGNATURES | PackageManager.GET_ACTIVITIES;
    private final Object store;
    private final Class<?> type;
    private final Context context;
    private final File fixtures;

    private PackageStoreProbe(ArtApplication running, File fixtures) throws Exception {
        type = Class.forName("com.trebledot.stone.runtime.ArtPackageStore");
        java.lang.reflect.Constructor<?> constructor = type.getDeclaredConstructor(ArtPackage.class);
        constructor.setAccessible(true);
        store = constructor.newInstance(running.apk);
        context = running.context;
        this.fixtures = fixtures;
    }

    static void run(ArtApplication running) throws Exception {
        String root = System.getProperty("stone.probe.archives");
        if (root == null) return;
        new PackageStoreProbe(running, new File(root)).verify(running);
    }

    private Object call(String name, Class<?>[] parameters, Object... arguments) throws Exception {
        Method method = type.getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        try { return method.invoke(store, arguments); }
        catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof Exception) throw (Exception) cause;
            throw new AssertionError("package store failed", cause);
        }
    }

    private PackageInfo install(String variant) throws Exception {
        return (PackageInfo) call("install", new Class<?>[]{Context.class, String.class, String.class, String.class},
            context, new File(fixtures, variant + ".apk").getPath(), context.getPackageName(), variant.equals("other-package") ? OTHER : PACKAGE);
    }

    private void remove(String name) throws Exception { call("remove", new Class<?>[]{String.class}, name); }
    private PackageInfo query() throws Exception { return context.getPackageManager().getPackageInfo(PACKAGE, FLAGS); }
    private void reject(String variant) throws Exception {
        reject(variant, -1);
    }
    private void reject(String variant, int status) throws Exception {
        String previous = query().applicationInfo.sourceDir;
        int directories = new File("/data/app/stone").list().length;
        try { install(variant); throw new AssertionError("package replacement accepted: " + variant); }
        catch (IOException expected) {
            if (status != -1) {
                java.lang.reflect.Field field = expected.getClass().getDeclaredField("status");
                field.setAccessible(true);
                if (field.getInt(expected) != status) throw new AssertionError("Incorrect install failure status: " + variant, expected);
            }
        }
        if (!previous.equals(query().applicationInfo.sourceDir)) throw new AssertionError("failed install changed package registration");
        if (new File("/data/app/stone").list().length != directories) throw new AssertionError("failed install leaked staged code");
    }

    private void verify(ArtApplication running) throws Exception {
        verifyOpenFileRemoval();
        File marker = new File(context.getFilesDir(), "package-store-recovery.properties");
        if (marker.isFile()) {
            Properties expected = new Properties();
            try (FileInputStream input = new FileInputStream(marker)) { expected.load(input); }
            PackageInfo restored = query();
            if (restored.getLongVersionCode() != 8 || restored.applicationInfo.uid != Integer.parseInt(expected.getProperty("uid"))
                    || restored.firstInstallTime != Long.parseLong(expected.getProperty("first"))
                    || !restored.applicationInfo.sourceDir.equals(expected.getProperty("source"))
                    || restored.signingInfo == null || !sha256(new File(restored.applicationInfo.sourceDir)).equals(
                        expected.getProperty("sha256", sha256(new File(fixtures, "update.apk")))))
                throw new AssertionError("installed package recovery across ART processes");
            System.out.println("ART recovered installed package code, UID, signing identity and timestamps from a previous process.");
        }
        remove(PACKAGE);
        remove(OTHER);
        PackageManager manager = context.getPackageManager();
        try { query(); throw new AssertionError("removed package still resolves"); }
        catch (PackageManager.NameNotFoundException expected) { }
        PackageInfo first = verifyPreparedCandidate();
        PackageInfo visible = query();
        if (first.applicationInfo.uid <= 10000 || visible.applicationInfo.uid != first.applicationInfo.uid
                || visible.firstInstallTime <= 0 || visible.lastUpdateTime < visible.firstInstallTime
                || visible.activities[0].applicationInfo.uid != visible.applicationInfo.uid
                || !new File(visible.applicationInfo.sourceDir).isFile()
                || manager.getInstalledPackages(FLAGS).stream().noneMatch(info -> PACKAGE.equals(info.packageName))
                || !context.getPackageName().equals(manager.getInstallSourceInfo(PACKAGE).getInstallingPackageName())
                || manager.getApplicationInfo(PACKAGE, 0).uid != first.applicationInfo.uid
                || !java.util.Arrays.asList(manager.getPackagesForUid(first.applicationInfo.uid)).contains(PACKAGE))
            throw new AssertionError("installed package public metadata");
        visible.versionName = "mutated";
        visible.signingInfo.getApkContentsSigners()[0] = null;
        if (query().signingInfo.getApkContentsSigners()[0] == null || "mutated".equals(query().versionName))
            throw new AssertionError("installed metadata cache escaped ownership");
        reject("other");
        reject("downgrade");
        reject("tampered-v2");
        File userData = new File(first.applicationInfo.dataDir, "package-store-probe.data");
        try (FileOutputStream output = new FileOutputStream(userData)) { output.write(42); }
        reject("future", android.content.pm.PackageInstaller.STATUS_FAILURE_INCOMPATIBLE);
        reject("native-32", android.content.pm.PackageInstaller.STATUS_FAILURE_INCOMPATIBLE);
        PackageInfo update = install("update");
        if (update.getLongVersionCode() != 8 || update.applicationInfo.uid != first.applicationInfo.uid
                || update.firstInstallTime != first.firstInstallTime || !new File(first.applicationInfo.sourceDir).isFile()
                || query().getLongVersionCode() != 8)
            throw new AssertionError("package upgrade, stable UID and cache invalidation");
        try (FileInputStream input = new FileInputStream(userData)) {
            if (input.read() != 42 || input.read() != -1) throw new AssertionError("package upgrade lost user data");
        }
        verifyFailedPublication();
        remove(PACKAGE);
        if (manager.getPackagesForUid(first.applicationInfo.uid) != null) throw new AssertionError("removed UID remained cached");
        install("v2");
        reject("rotated-no-data");
        PackageInfo rotated = install("rotated");
        if (!rotated.signingInfo.hasPastSigningCertificates()) throw new AssertionError("package rotation history lost");
        int capabilities = (Integer) android.content.pm.Signature.class.getMethod("getFlags")
            .invoke(query().signingInfo.getSigningCertificateHistory()[0]);
        if ((capabilities & 1) == 0) throw new AssertionError("persisted signing rotation capability lost");
        reject("v2");
        remove(PACKAGE);
        install("multi");
        if (!install("multi").signingInfo.hasMultipleSigners()) throw new AssertionError("multi-signer replacement lost identity");
        reject("other");
        remove(PACKAGE);
        verifyConcurrency(running);
        remove(OTHER);
        update = install("update");
        Properties saved = new Properties();
        saved.setProperty("uid", Integer.toString(update.applicationInfo.uid));
        saved.setProperty("first", Long.toString(update.firstInstallTime));
        saved.setProperty("source", update.applicationInfo.sourceDir);
        saved.setProperty("sha256", sha256(new File(update.applicationInfo.sourceDir)));
        try (FileOutputStream output = new FileOutputStream(marker)) { saved.store(output, "Package store recovery"); output.getFD().sync(); }
        System.out.println("ART published installed APKs atomically, enforced signer/version replacement, preserved UIDs and serialized concurrent writers.");
    }

    private PackageInfo verifyPreparedCandidate() throws Exception {
        File incoming = new File(context.getCacheDir(), "incoming-package.apk");
        copy(new File(fixtures, "v2.apk"), incoming);
        Object candidate = call("prepare", new Class<?>[]{Context.class, String.class, String.class}, context, incoming.getPath(), PACKAGE);
        try (java.io.Closeable cleanup = (java.io.Closeable) candidate) {
            try { query(); throw new AssertionError("uncommitted package candidate became visible"); }
            catch (PackageManager.NameNotFoundException expected) { }
            copy(new File(fixtures, "other.apk"), incoming);
            PackageInfo installed = (PackageInfo) call("commit", new Class<?>[]{candidate.getClass(), String.class}, candidate, context.getPackageName());
            if (!sha256(new File(installed.applicationInfo.sourceDir)).equals(sha256(new File(fixtures, "v2.apk"))))
                throw new AssertionError("replacing the download changed the approved install candidate");
            return installed;
        } finally { if (!incoming.delete()) throw new AssertionError("incoming package fixture cleanup"); }
    }

    private void verifyOpenFileRemoval() throws Exception {
        File file = new File(context.getCacheDir(), "package-delete-open.apk");
        try (FileOutputStream output = new FileOutputStream(file)) { output.write(new byte[]{1, 2, 3}); }
        try (FileInputStream old = new FileInputStream(file)) {
            if (!file.delete() || file.exists()) throw new AssertionError("unlink of an open APK");
            try (FileOutputStream output = new FileOutputStream(file)) { output.write(42); }
            if (old.read() != 1 || old.read() != 2 || old.read() != 3 || old.read() != -1)
                throw new AssertionError("unlinked descriptor lost original bytes");
            try (FileInputStream replaced = new FileInputStream(file)) {
                if (replaced.read() != 42 || replaced.read() != -1) throw new AssertionError("unlinked path cannot be reused");
            }
        } finally { if (file.exists() && !file.delete()) throw new AssertionError("open APK fixture cleanup"); }
    }

    private void verifyFailedPublication() throws Exception {
        File index = new File("/data/system/stone-packages/packages.properties");
        String previous = query().applicationInfo.sourceDir;
        if (!index.setReadOnly()) throw new AssertionError("package registry read-only failure fixture");
        try {
            try { install("update"); throw new AssertionError("failed registry publication reported success"); }
            catch (IOException expected) { }
        } finally {
            if (!index.setWritable(true)) throw new AssertionError("package registry failure fixture cleanup");
        }
        if (!query().applicationInfo.sourceDir.equals(previous)) throw new AssertionError("failed package registry update replaced previous code");
    }

    private void verifyConcurrency(ArtApplication running) throws Exception {
        java.util.concurrent.CountDownLatch start = new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.atomic.AtomicReference<Throwable> failure = new java.util.concurrent.atomic.AtomicReference<>();
        Thread[] threads = new Thread[2];
        for (int i = 0; i < 2; i++) {
            final String variant = i == 0 ? "v2" : "other-package";
            threads[i] = new Thread(() -> {
                try { start.await(); new PackageStoreProbe(running, fixtures).install(variant); }
                catch (Throwable error) { failure.compareAndSet(null, error); }
            }, "Stone package transaction " + i);
            threads[i].setDaemon(true);
            threads[i].start();
        }
        start.countDown();
        for (Thread thread : threads) { thread.join(30000); if (thread.isAlive()) throw new AssertionError("package transaction lock deadlocked"); }
        if (failure.get() != null) throw new AssertionError("concurrent package transaction failed", failure.get());
        PackageInfo other = context.getPackageManager().getPackageInfo(OTHER, 0);
        if (other.applicationInfo.uid == query().applicationInfo.uid) throw new AssertionError("concurrent package installs reused UID");
    }

    private static String sha256(File file) throws Exception {
        java.security.MessageDigest digest = java.security.MessageDigest.getInstance("SHA-256");
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[8192]; int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        StringBuilder text = new StringBuilder();
        for (byte value : digest.digest()) text.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }

    private static void copy(File source, File target) throws IOException {
        try (FileInputStream input = new FileInputStream(source); FileOutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[8192]; int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
        }
    }
}
