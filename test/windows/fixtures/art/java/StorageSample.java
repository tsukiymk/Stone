package com.trebledot.stone.probe;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Environment;
import android.os.Parcel;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.io.File;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

/** Real image storage APIs and persistent app-specific external file I/O. */
public final class StorageSample {
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static int run(AssetManager assets, String stage) throws Exception {
        boolean privateStorage = stage.equals("private-write") || stage.equals("private-read");
        check(privateStorage || stage.equals("write") || stage.equals("read"), "Unknown storage probe stage");
        boolean write = stage.equals("write") || stage.equals("private-write");
        ArtPackage apk = ArtPackage.openApplication(assets, "/data/application.apk");
        ArtApplication application = ArtApplication.start(apk);
        Context context = application.context;
        PackageStoreProbe.run(application);
        check(context.getPackageManager().getInstallerPackageName(context.getPackageName()) == null,
            "Direct APK launch must not fabricate an installing package");
        Context device = context.createDeviceProtectedStorageContext();
        check(device.isDeviceProtectedStorage() && device.getDataDir().isDirectory(), "Device-protected data root unavailable");
        check(!context.getDataDir().equals(device.getDataDir()) && device.getFilesDir().isDirectory()
            && device.getCacheDir().isDirectory(), "Device-protected storage directories unavailable or shared");
        android.content.SharedPreferences preferences = device.getSharedPreferences("storage-restart", Context.MODE_PRIVATE);
        File deviceFile = new File(device.getFilesDir(), "storage-restart.bin");
        byte[] deviceMarker = "Stone device-protected storage restart".getBytes("UTF-8");
        if (write) {
            check(!preferences.contains("persisted") && !deviceFile.exists(), "Write stage requires fresh device-protected data");
            check(preferences.edit().putString("persisted", "device-protected").commit(), "Device-protected preferences commit failed");
            Files.write(deviceFile.toPath(), deviceMarker);
        }
        check("device-protected".equals(preferences.getString("persisted", null))
            && Arrays.equals(Files.readAllBytes(deviceFile.toPath()), deviceMarker), "Device-protected data was not retained");
        check(!context.getSharedPreferences("storage-restart", Context.MODE_PRIVATE).contains("persisted"),
            "Device-protected preferences leaked into credential-protected storage");
        System.out.println("PASS device-protected directories, preferences and file bytes, " + stage + " stage; install-source query contracts.");
        if (privateStorage) return 42;
        File root = Environment.getExternalStorageDirectory();
        StorageManager manager = context.getSystemService(StorageManager.class);
        List<StorageVolume> volumes = manager.getStorageVolumes();
        check(volumes.size() == 1, "One mounted Stone storage volume required");
        StorageVolume volume = volumes.get(0);
        check(root.isDirectory() && root.canWrite(), "Advertised storage root is not writable");
        check(volume.isPrimary() && volume.isEmulated() && !volume.isRemovable(), "Incorrect storage volume attributes");
        check(Environment.MEDIA_MOUNTED.equals(volume.getState())
            && Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState(root)), "Storage is not mounted");
        check(root.equals(volume.getDirectory()) && manager.getStorageVolume(root).isPrimary(), "Storage path lookup mismatch");
        check(Environment.isExternalStorageEmulated(root) && !Environment.isExternalStorageRemovable(root), "Environment attributes mismatch");
        Parcel parcel = Parcel.obtain();
        try {
            volume.writeToParcel(parcel, 0); parcel.setDataPosition(0);
            StorageVolume restored = StorageVolume.CREATOR.createFromParcel(parcel);
            check(restored.getDirectory().equals(root) && restored.isPrimary()
                && Environment.MEDIA_MOUNTED.equals(restored.getState()), "StorageVolume parcel round trip");
        } finally { parcel.recycle(); }
        File[] files = context.getExternalFilesDirs(null), caches = context.getExternalCacheDirs();
        check(files.length == 1 && files[0] != null && files[0].isDirectory(), "External files directory unavailable");
        check(caches.length == 1 && caches[0] != null && caches[0].isDirectory(), "External cache directory unavailable");
        String appRoot = root.getCanonicalPath() + "/Android/data/" + context.getPackageName() + "/";
        check(files[0].getCanonicalPath().equals(appRoot + "files")
            && caches[0].getCanonicalPath().equals(appRoot + "cache"), "App-specific directories escaped their root");
        check(context.getExternalFilesDir(Environment.DIRECTORY_PICTURES).isDirectory(), "Typed external directory unavailable");
        byte[] marker = "Stone real external storage restart".getBytes("UTF-8");
        File persisted = new File(files[0], "storage-restart.bin");
        if (stage.equals("write")) {
            check(!persisted.exists(), "Write stage requires fresh probe data");
            Files.write(persisted.toPath(), marker);
        }
        check(Arrays.equals(Files.readAllBytes(persisted.toPath()), marker), "External storage bytes were not retained");
        Class<?> sm = Class.forName("android.os.ServiceManager"), iface = Class.forName("android.os.storage.IStorageManager");
        android.os.IBinder binder = (android.os.IBinder) sm.getMethod("getService", String.class).invoke(null, "mount");
        Object service = binder.queryLocalInterface(iface.getName());
        int user = (Integer) android.os.UserHandle.class.getMethod("myUserId").invoke(null);
        java.lang.reflect.Method get = iface.getMethod("getVolumeList", int.class, String.class, int.class);
        deny(() -> get.invoke(service, user + 1, context.getPackageName(), 0));
        deny(() -> get.invoke(service, user, "com.other.storage", 0));
        java.lang.reflect.Method mkdirs = iface.getMethod("mkdirs", String.class, String.class);
        deny(() -> mkdirs.invoke(service, context.getPackageName(), "/data/apps/forbidden-storage"));
        System.out.println("PASS image storage volume, Environment, external files/cache paths, parcel, actual bytes, "
            + stage + " stage and cross-user/package/path denial.");
        return 42;
    }
    private interface Query { void run() throws Exception; }
    private static void deny(Query query) throws Exception {
        try { query.run(); throw new AssertionError("Unauthorized storage query succeeded"); }
        catch (java.lang.reflect.InvocationTargetException error) {
            if (!(error.getCause() instanceof SecurityException)) throw error;
        }
    }
}
