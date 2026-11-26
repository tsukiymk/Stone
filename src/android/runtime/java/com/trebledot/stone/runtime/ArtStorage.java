package com.trebledot.stone.runtime;

import android.os.Binder;
import android.os.Environment;
import android.os.UserHandle;
import android.os.storage.StorageVolume;
import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Properties;
import android.util.AtomicFile;

/** An emulated volume backed by this Stone profile's writable Android data mount. */
final class ArtStorage {
    private final int uid, user;
    private final String packageName;
    private final File root;
    private final StorageVolume volume;
    private final ArtPackage apk;
    private final String identity;
    private volatile boolean allFiles;
    private final java.util.concurrent.ScheduledExecutorService worker=java.util.concurrent.Executors.newSingleThreadScheduledExecutor(task->{
        Thread thread=new Thread(task,"Stone shared-storage permissions"); thread.setDaemon(true); return thread;
    });
    private static native void configureNative(String packageName, boolean allFiles) throws IOException;
    private static native String dataIdentityNative(String directory) throws IOException;

    ArtStorage(ArtPackage apk, int user) throws ReflectiveOperationException {
        this.uid = apk.applicationInfo.uid;
        this.user = user;
        this.packageName = apk.applicationInfo.packageName;
        this.apk = apk;
        root = new File("/storage/emulated/0");
        try {
            identity = identity(apk.packageInfo);
            allFiles = readAllowed();
            configureNative(packageName, allFiles);
        } catch (IOException error) { throw new IllegalStateException("Cannot prepare Stone external storage",error); }
        Class<?> builderType = Class.forName("android.os.storage.StorageVolume$Builder");
        Object builder = builderType.getConstructor(String.class, File.class, String.class, UserHandle.class, String.class)
            .newInstance("emulated;" + user, root, "Stone storage", android.os.Process.myUserHandle(), Environment.MEDIA_MOUNTED);
        builderType.getMethod("setPrimary", boolean.class).invoke(builder, true);
        builderType.getMethod("setEmulated", boolean.class).invoke(builder, true);
        volume = (StorageVolume) builderType.getMethod("build").invoke(builder);
        worker.scheduleWithFixedDelay(()->{
            try { synchronized(this) { boolean allowed=readAllowed(); if (allowed!=allFiles) apply(allowed); } }
            catch (IOException | RuntimeException error) { android.util.Log.w("StoneStorage","Cannot reload storage permissions",error); }
        },1,1,java.util.concurrent.TimeUnit.SECONDS);
    }

    static String identity(android.content.pm.PackageInfo info) throws IOException {
        return info.applicationInfo.uid + ":" + dataIdentityNative(info.applicationInfo.dataDir);
    }
    private boolean readAllowed() throws IOException {
        Properties saved=new Properties(); AtomicFile file=policy();
        if (file.getBaseFile().isFile()) {
            if (file.getBaseFile().length()>65536) throw new IOException("Storage permission policy exceeds its limit");
            try(FileInputStream input=new FileInputStream(file.getBaseFile())) { saved.load(input); }
        }
        return declared() && identity.equals(saved.getProperty("identity")) && "true".equals(saved.getProperty("allowed"));
    }

    boolean declared() {
        return apk.packageInfo.requestedPermissions != null && java.util.Arrays.asList(apk.packageInfo.requestedPermissions)
            .contains("android.permission.MANAGE_EXTERNAL_STORAGE");
    }
    boolean allFiles() { return declared() && allFiles; }
    private AtomicFile policy() { return new AtomicFile(new File("/data/system/stone-storage-access",packageName+".properties")); }
    synchronized void setAllFiles(boolean allowed) throws IOException {
        if (allowed && !declared()) throw new SecurityException("Application did not declare all-files access");
        Properties saved = new Properties(); saved.setProperty("identity",identity); saved.setProperty("allowed",Boolean.toString(allowed));
        AtomicFile policy = policy(); File parent=policy.getBaseFile().getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create storage permission directory");
        try (java.io.RandomAccessFile gate=new java.io.RandomAccessFile(new File(parent,packageName+".lock"),"rw");
                java.nio.channels.FileLock lock=gate.getChannel().lock(0,1,false)) {
            FileOutputStream output = policy.startWrite();
            try { saved.store(output,"Stone shared-storage access"); policy.finishWrite(output); }
            catch (IOException | RuntimeException error) { policy.failWrite(output); throw error; }
        }
        apply(allowed);
    }
    private void apply(boolean allowed) throws IOException {
        configureNative(packageName, allowed); allFiles = allowed;
        try { android.app.AppOpsManager.class.getMethod("invalidateAppOpModeCache").invoke(null); }
        catch (ReflectiveOperationException error) { throw new IOException("Cannot invalidate image storage access cache",error); }
        android.util.Log.i("StoneStorage","All-files access " + (allowed ? "allowed" : "denied") + " for " + packageName);
    }
    static void forget(String name) { new AtomicFile(new File("/data/system/stone-storage-access",name+".properties")).delete(); }

    Object call(Object instance, Method method, Object[] arguments) {
        switch (method.getName()) {
            case "getVolumeList":
                caller((String) arguments[1]);
                // Images use either a user ID or a calling UID in this position.
                if ((Integer) arguments[0] != user && (Integer) arguments[0] != uid)
                    throw new SecurityException("Storage volumes are only available for the current Stone user");
                return new StorageVolume[]{volume};
            case "mkdirs":
                caller((String) arguments[0]);
                File directory = new File((String) arguments[1]);
                String path = canonical(directory);
                String files = canonical(new File(root, "Android/data/" + packageName));
                String obb = canonical(new File(root, "Android/obb/" + packageName));
                if (!within(path, files) && !within(path, obb))
                    throw new SecurityException("Storage directory is outside the current application's external data");
                if (!directory.isDirectory() && !directory.mkdirs())
                    throw new IllegalStateException("Cannot create application external directory");
                return null;
            default: throw new UnsupportedOperationException("ART storage service: " + method.getName());
        }
    }
    private void caller(String name) {
        if (Binder.getCallingUid() != uid || !packageName.equals(name))
            throw new SecurityException("Storage request does not belong to the current Stone application");
    }
    private static boolean within(String path, String parent) { return path.equals(parent) || path.startsWith(parent + "/"); }
    private static String canonical(File file) {
        try { return file.getCanonicalPath(); }
        catch (IOException error) { throw new IllegalStateException("Cannot resolve application storage path", error); }
    }
}
