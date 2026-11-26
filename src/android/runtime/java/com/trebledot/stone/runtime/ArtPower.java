package com.trebledot.stone.runtime;

import android.os.IBinder;
import android.os.IInterface;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.WorkSource;
import java.lang.reflect.Method;
import java.util.IdentityHashMap;
import java.util.Map;

/** Per-APK wake lock tokens; the user Service owns the Windows sleep inhibitor. */
final class ArtPower {
    private final ArtPackage apk;
    private final Map<IBinder, IInterface> locks = new IdentityHashMap<>();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final boolean permission;
    private int jobs;
    private volatile boolean changing, required;
    boolean hasRequests() { return changing || required; }
    private void request(boolean value) {
        changing = true;
        try { setRequired(value); required = value; }
        finally { changing = false; }
    }
    ArtPower(ArtPackage apk) {
        this.apk = apk;
        permission = apk.packageInfo.requestedPermissions != null
            && java.util.Arrays.asList(apk.packageInfo.requestedPermissions).contains("android.permission.WAKE_LOCK");
    }
    private static native void setRequired(boolean required);
    private static native boolean saver();
    synchronized void holdJob() {
        if (jobs == Integer.MAX_VALUE) throw new IllegalStateException("Too many job power requests");
        if (jobs == 0 && locks.isEmpty()) request(true);
        jobs++;
    }
    synchronized void releaseJob() {
        if (jobs <= 0) throw new IllegalStateException("Job power request underflow");
        if (jobs == 1 && locks.isEmpty()) request(false);
        jobs--;
    }
    private void notify(IInterface callback, boolean held) {
        if (callback == null) return;
        main.post(() -> {
            try { Class.forName("android.os.IWakeLockCallback").getMethod("onStateChanged", boolean.class).invoke(callback, held); }
            catch (ReflectiveOperationException error) { android.util.Log.w("StonePower", "Wake lock callback failed", error); }
        });
    }
    private void attribution(WorkSource source) throws ReflectiveOperationException {
        if (source != null && !(Boolean) WorkSource.class.getMethod("isEmpty").invoke(source))
            throw new SecurityException("Cross-UID wake lock attribution is unavailable");
    }
    private void ownPermission() {
        if (!permission) throw new SecurityException("Application must declare android.permission.WAKE_LOCK");
    }
    synchronized Object call(Object instance, Method method, Object[] args) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "acquireWakeLock":
            case "acquireWakeLockAsync":
            case "acquireWakeLockWithUid": {
                ownPermission();
                if (!apk.applicationInfo.packageName.equals(args[3])) throw new SecurityException("Wake lock package mismatch");
                if ((Integer) args[1] != PowerManager.PARTIAL_WAKE_LOCK)
                    throw new IllegalArgumentException("Only partial wake locks are supported");
                if (method.getName().equals("acquireWakeLockWithUid")) {
                    if ((Integer) args[4] != apk.applicationInfo.uid) throw new SecurityException("Wake lock UID mismatch");
                } else attribution((WorkSource) args[4]);
                IBinder token = (IBinder) args[0];
                if (token == null) throw new IllegalArgumentException("Wake lock token is required");
                boolean existing = locks.containsKey(token);
                if (!existing && locks.size() >= 1024) throw new IllegalStateException("Too many wake locks");
                if (locks.isEmpty() && jobs == 0) request(true);
                int callbackIndex = method.getName().equals("acquireWakeLockWithUid") ? 6 : 7;
                IInterface callback = args.length > callbackIndex ? (IInterface) args[callbackIndex] : null;
                locks.put(token, callback); notify(callback, true); return null;
            }
            case "releaseWakeLock":
            case "releaseWakeLockAsync": {
                ownPermission(); IBinder token = (IBinder) args[0];
                if (!locks.containsKey(token)) return null;
                if (locks.size() == 1 && jobs == 0) request(false);
                IInterface callback = locks.remove(token); notify(callback, false); return null;
            }
            case "updateWakeLockWorkSource": ownPermission(); attribution((WorkSource) args[1]); return null;
            case "updateWakeLockCallback": {
                ownPermission(); IBinder token = (IBinder) args[0];
                if (!locks.containsKey(token)) throw new IllegalArgumentException("Wake lock is not held");
                IInterface callback = (IInterface) args[1]; locks.put(token, callback); notify(callback, true); return null;
            }
            case "isWakeLockLevelSupported":
            case "isWakeLockLevelSupportedWithDisplayId": return (Integer) args[0] == PowerManager.PARTIAL_WAKE_LOCK;
            case "isPowerSaveMode": return saver();
            case "isBatterySaverSupported": return true;
            // Stone does not apply Android Doze or individual BatterySaverPolicy restrictions.
            case "areAutoPowerSaveModesEnabled":
            case "isDeviceIdleMode":
            case "isLightDeviceIdleMode":
            case "isLowPowerStandbySupported":
            case "isLowPowerStandbyEnabled": return false;
            case "isExemptFromLowPowerStandby": return true;
            case "getPowerSaveState": {
                Class<?> builder = Class.forName("android.os.PowerSaveState$Builder"); Object state = builder.getConstructor().newInstance();
                builder.getMethod("setGlobalBatterySaverEnabled", boolean.class).invoke(state, saver());
                builder.getMethod("setBrightnessFactor", float.class).invoke(state, 1.0f);
                return builder.getMethod("build").invoke(state);
            }
            default: throw new UnsupportedOperationException("ART power service: " + method.getName());
        }
    }
}
