package com.trebledot.stone.cit.nativeapp;

import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Build;
import android.util.Log;
import android.view.Surface;
import java.io.File;

public final class NativeActivity {
    private static native int registered(int value);
    private static native double mixed(float a, int b, double c, int d, int e);
    private static native int dynamic();
    private static native int nativeAndroid(AssetManager assets, Surface surface);

    private static int javaCallback(int value) {
        return value + 5;
    }

    public static String verify(Activity activity) {
        if (!"x86_64".equals(Build.SUPPORTED_ABIS[0])) throw new AssertionError("Wrong guest ABI");
        try {
            System.loadLibrary("missing_native_test");
            throw new AssertionError("Missing library was accepted");
        } catch (UnsatisfiedLinkError expected) {
            Log.i("WindowsNative", expected.getMessage());
        }
        System.loadLibrary("native_test");
        Runtime.getRuntime().loadLibrary("native_test");
        String installed = new File(activity.getApplicationInfo().nativeLibraryDir,
                System.mapLibraryName("native_test")).getAbsolutePath();
        System.load(installed);
        Runtime.getRuntime().load(activity.getApplicationInfo().sourceDir + "!/lib/x86_64/libnative_test.so");
        int result = registered(7);
        double mixedResult = mixed(1.5f, 2, 3.0, 4, 13);
        if (result != 42 || mixedResult != 23.5) {
            throw new AssertionError("Unexpected JNI values " + result + ", " + mixedResult);
        }
        int linked = dynamic();
        if (linked != 42) throw new AssertionError("Unexpected libdl result " + linked);
        Log.i("WindowsNative", "libdl result=" + linked);
        Surface surface = new Surface((android.graphics.SurfaceTexture) null);
        try {
            int nativeResult = nativeAndroid(activity.getAssets(), surface);
            if (nativeResult != 42) throw new AssertionError("libandroid result " + nativeResult);
        } finally {
            surface.release();
        }
        Log.i("WindowsNative", "libandroid asset and software window verified");
        String message = "result=" + result + " mixed=" + mixedResult;
        Log.i("WindowsNative", message);
        return "x86_64 ELF, JNI registration and callbacks, libdl and NDK assets";
    }
}
