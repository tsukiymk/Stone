package com.trebledot.stone.cit.nativejit;

import android.os.Build;
import android.util.Log;

public final class JitActivity {
    private static native int add(int left, int right);
    private static native long addWide(long left, int right);
    private static native long mix(long first, long second, long third, long fourth);
    private static native long zeroExtend(int value);
    private static native int registeredLeaf(int value);
    private static native String registeredString();
    private static native double registeredDouble(float marker, int unused, double value);
    private static native long registeredStack(long first, long a, long b, long c, long d, long e, long seventh);

    public static String verify() {
        String abi = Build.SUPPORTED_ABIS[0];
        if (!"arm64-v8a".equals(abi)) throw new AssertionError("Wrong guest ABI: " + abi);
        System.loadLibrary("native_jit_test");
        int sum = add(8, 13);
        long wide = addWide(0x1_0000_0000L, -42);
        if (sum != 42 || add(-21, 0) != 0 || wide != 0xffff_ffd6L) {
            throw new AssertionError("Unexpected JNI values " + sum + ", " + wide);
        }
        long mixed = mix(0xfedc_ba98_7654_3210L, 3, 0x1_0000_0000L, 17);
        if (mixed != ((0xfedc_ba98_7654_3210L + 6) ^ (0x1_0000_0000L - 17))
                || zeroExtend(-1) != 0xffff_ffffL) {
            throw new AssertionError("Incorrect JNI stack arguments or integer width");
        }
        if (registeredLeaf(35) != 42 || !"arm64 registered 42".equals(registeredString())
                || registeredDouble(1.5f, 99, -7.25) != -7.25
                || registeredStack(19, 1, 2, 3, 4, 5, 23) != 42
                || registeredStack(5, 0, 0, 0, 0, 0, 37) != 42) {
            throw new AssertionError("Incorrect dynamically registered JNI results");
        }
        Log.i("WindowsNativeJit", abi + " JNI results verified");
        return "arm64-v8a ELF, translated JNI calls and native registration";
    }
}
