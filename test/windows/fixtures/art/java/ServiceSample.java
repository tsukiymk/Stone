package com.trebledot.stone.probe;

import android.app.ActivityManager;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.util.Map;

/** Optional original-APK foreground service check, separate from CIT data and assertions. */
public final class ServiceSample {
    @SuppressWarnings("deprecation")
    public static int run(AssetManager assets, String serviceClass) throws Exception {
        java.lang.reflect.Method initialize = Class.forName("com.trebledot.stone.runtime.ArtConfiguration").getDeclaredMethod("initialize");
        initialize.setAccessible(true); initialize.invoke(null);
        ArtPackage apk = ArtPackage.openApplication(assets, "/data/application.apk");
        ArtApplication running = ArtApplication.start(apk);
        Context context = running.context;
        ComponentName name = new ComponentName(context.getPackageName(), serviceClass);
        Intent request = new Intent().setComponent(name);
        if (!name.equals(context.startForegroundService(request))) throw new AssertionError("sample service did not resolve");
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        long limit = android.os.SystemClock.uptimeMillis() + 30000;
        boolean foreground = false;
        do {
            BroadcastProbe.drain();
            for (ActivityManager.RunningServiceInfo info : manager.getRunningServices(20))
                if (name.equals(info.service) && info.started && info.foreground) foreground = true;
            if (foreground) break;
            Thread.sleep(5);
        } while (android.os.SystemClock.uptimeMillis() < limit);
        if (!foreground) throw new AssertionError("sample onStartCommand did not enter foreground state");
        Class<?> thread = Class.forName("android.app.ActivityThread");
        Object instance = thread.getMethod("currentActivityThread").invoke(null);
        java.lang.reflect.Field field = thread.getDeclaredField("mServices"); field.setAccessible(true);
        @SuppressWarnings("unchecked") Map<Object, Service> live = (Map<Object, Service>) field.get(instance);
        Service found = null;
        for (Service service : live.values()) if (serviceClass.equals(service.getClass().getName())) found = service;
        if (found == null || found.getApplication() != running.application) throw new AssertionError("sample image ActivityThread Service ownership");
        System.out.println("ART original APK service started: " + name.flattenToString() + " foregroundType=" + found.getForegroundServiceType());
        if (!context.stopService(request)) throw new AssertionError("sample stopService failed");
        Service stopped = found;
        limit = android.os.SystemClock.uptimeMillis() + 30000;
        do { BroadcastProbe.drain(); if (!live.containsValue(stopped)) break; Thread.sleep(5); }
        while (android.os.SystemClock.uptimeMillis() < limit);
        if (live.containsValue(stopped) || !manager.getRunningServices(20).isEmpty()) throw new AssertionError("sample service was not destroyed");
        System.out.println("ART original APK service stopped and was removed from the image ActivityThread.");
        return 42;
    }
}
