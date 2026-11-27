package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.res.AssetManager;

/** Starts the original APK on the selected image's ART and AOSP framework. */
public final class ArtBoot {
    private ArtBoot() {}
    static native void lockApplication(String packageName);
    static native void bindApplicationUid(int uid);
    static native void setProcessName(String name);
    private static native void registerLaunchProfile(String name) throws java.io.IOException;
    static native void bindNativeLibraries(ClassLoader loader, String directory,String dataDirectory) throws java.io.IOException;
    static native long[] memoryInfo();
    static native int glEsVersion();
    private static native void prepareAot(String source) throws java.io.IOException;

    public static int runInstalled(String name) throws Exception {
        return runInstalled(name, null);
    }
    public static int runInstalledSession(String name, boolean background) throws Exception {
        return runInstalled(name, background);
    }
    private static int runInstalled(String name, Boolean background) throws Exception {
        ArtPackageStore.validateName(name);
        android.content.pm.PackageInfo info = new ArtPackageStore(null).get(name, 0);
        if (info == null || info.applicationInfo == null) throw new java.io.IOException("Application is not installed: " + name);
        String source = info.applicationInfo.sourceDir;
        return run(source, background);
    }

    public static int run(String source) throws Exception {
        return run(source, null);
    }
    public static int runSession(String source, boolean background) throws Exception {
        return run(source, background);
    }
    private static int run(String source, Boolean background) throws Exception {
        ArtConfiguration.initialize();
        java.lang.reflect.Constructor<AssetManager> constructor = AssetManager.class.getDeclaredConstructor();
        constructor.setAccessible(true);
        ArtPackage apk = ArtPackage.openCurrentApplication(constructor.newInstance(), source);
        // The system class path may contain the explicitly supplied older APK.
        // Delegate only to the runtime/framework loader, so old Application or
        // Activity classes cannot shadow the selected installed version.
        ArtNativeLibraries.prepare(apk.applicationInfo);
        prepareAot(apk.applicationInfo.sourceDir);
        ClassLoader loader = new dalvik.system.PathClassLoader(apk.applicationInfo.sourceDir,
            apk.applicationInfo.nativeLibraryDir, ArtBoot.class.getClassLoader());
        if (apk.applicationInfo.nativeLibraryDir != null) bindNativeLibraries(loader, apk.applicationInfo.nativeLibraryDir,apk.applicationInfo.dataDir);
        if (background != null) registerLaunchProfile(apk.applicationInfo.packageName);
        ArtApplication application = ArtApplication.start(apk, loader);
        try {
            if (background != null) ArtSession.run(application, background);
            else {
                Activity activity = application.startActivity();
                float density = activity.getResources().getDisplayMetrics().density;
                ArtWindow.run(activity, Math.max(1, Math.round(360 * density)), Math.max(1, Math.round(640 * density)));
            }
        } finally {
            application.destroyActivity();
            // The host keeps the APK resources and its data lock until the process exits.
        }
        // ActivityThread normally drains pending framework writes when an Activity stops.
        Class.forName("android.app.QueuedWork").getMethod("waitToFinish").invoke(null);
        return 0;
    }
}
