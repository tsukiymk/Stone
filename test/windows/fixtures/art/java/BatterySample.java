package com.trebledot.stone.probe;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.os.BatteryManager;
import android.os.Looper;
import android.os.PowerManager;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/** Framework battery broadcasts and Windows host-state transitions. */
public final class BatterySample {
    private static native int[] host();
    private static native void fixture(int[] state);
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static Intent battery(Context context) { return context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED)); }
    private interface Condition { boolean ready() throws Exception; }
    private static void await(Condition condition) throws Exception {
        long deadline = android.os.SystemClock.uptimeMillis() + 15000;
        do { BroadcastProbe.drain(); if (condition.ready()) return; Thread.sleep(10); }
        while (android.os.SystemClock.uptimeMillis() < deadline);
        throw new AssertionError("Battery monitor/receiver timeout");
    }
    public static int run(AssetManager assets, String ignored) throws Exception {
        Method initialize = Class.forName("com.trebledot.stone.runtime.ArtConfiguration").getDeclaredMethod("initialize");
        initialize.setAccessible(true); initialize.invoke(null);
        ArtPackage apk = ArtPackage.openApplication(assets, "/data/application.apk");
        ArtApplication application = ArtApplication.start(apk);
        Context context = application.context;
        BatteryManager manager = context.getSystemService(BatteryManager.class);
        check(manager != null, "framework BatteryManager service");
        int[] initialHost = host();
        Intent initial = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        check(initial != null, "initial Windows battery broadcast");
        boolean absent = initialHost[1] != 255 && (initialHost[1] & 128) != 0;
        int hostLevel = absent ? 100 : initialHost[2] <= 100 ? initialHost[2] : -1;
        check(initial.getIntExtra(BatteryManager.EXTRA_LEVEL, -2) == hostLevel, "real Windows initial level");
        System.out.println("Windows battery snapshot: " + java.util.Arrays.toString(initialHost));
        Resources system = Resources.getSystem();
        int warning = system.getInteger(system.getIdentifier("config_lowBatteryWarningLevel", "integer", "android"));
        int close = warning + system.getInteger(system.getIdentifier("config_lowBatteryCloseWarningBump", "integer", "android"));
        System.out.println("Image battery warning/recovery: " + warning + "/" + close);
        initial.putExtra(BatteryManager.EXTRA_LEVEL, 999);
        check(context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            .getIntExtra(BatteryManager.EXTRA_LEVEL, -2) == hostLevel, "sticky snapshots are copies");
        check(manager.getLongProperty(BatteryManager.BATTERY_PROPERTY_ENERGY_COUNTER) == Long.MIN_VALUE,
            "unsupported energy telemetry must remain unknown");
        List<Intent> received = new ArrayList<>(); List<Boolean> sticky = new ArrayList<>();
        BroadcastReceiver receiver = new BroadcastReceiver() {
            public void onReceive(Context owner, Intent intent) {
                check(Looper.myLooper() == Looper.getMainLooper(), "battery callback thread");
                received.add(new Intent(intent)); sticky.add(isInitialStickyBroadcast());
            }
        };
        IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        for (String action : new String[]{Intent.ACTION_BATTERY_LOW, Intent.ACTION_BATTERY_OKAY, Intent.ACTION_POWER_CONNECTED,
                Intent.ACTION_POWER_DISCONNECTED, BatteryManager.ACTION_CHARGING, BatteryManager.ACTION_DISCHARGING,
                PowerManager.ACTION_POWER_SAVE_MODE_CHANGED}) filter.addAction(action);
        context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        try {
            await(() -> !received.isEmpty());
            check(received.size() == 1 && sticky.get(0), "initial sticky delivery");
            fixture(new int[]{1, 8, 70, 0});
            await(() -> manager.isCharging() && manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) == 70
                && battery(context).getIntExtra(BatteryManager.EXTRA_LEVEL, -1) == 70);
            int start = received.size();
            fixture(new int[]{0, 2, 10, 1});
            await(() -> manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) == 10
                && received.subList(start, received.size()).stream().anyMatch(intent -> Intent.ACTION_BATTERY_LOW.equals(intent.getAction())));
            check(!manager.isCharging(), "discharging state");
            for (String action : new String[]{Intent.ACTION_POWER_DISCONNECTED, BatteryManager.ACTION_DISCHARGING, PowerManager.ACTION_POWER_SAVE_MODE_CHANGED})
                await(() -> received.subList(start, received.size()).stream().anyMatch(intent -> action.equals(intent.getAction())));
            fixture(new int[]{0, 2, 15, 1});
            await(() -> battery(context).getIntExtra(BatteryManager.EXTRA_LEVEL, -1) == 15);
            fixture(new int[]{0, 2, 16, 1});
            await(() -> battery(context).getIntExtra(BatteryManager.EXTRA_LEVEL, -1) == 16);
            check(battery(context).getBooleanExtra(BatteryManager.EXTRA_BATTERY_LOW, false), "low-battery hysteresis");
            fixture(new int[]{0, 2, close, 1});
            await(() -> battery(context).getIntExtra(BatteryManager.EXTRA_LEVEL, -1) == close);
            check(!battery(context).getBooleanExtra(BatteryManager.EXTRA_BATTERY_LOW, true), "Windows low flag must not replace the image's recovery threshold");
            fixture(new int[]{0, 0, 60, 1});
            await(() -> manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) == 60);
            fixture(new int[]{1, 128, 255, 0});
            await(() -> manager.isCharging() && manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) == 100
                && !battery(context).getBooleanExtra(BatteryManager.EXTRA_PRESENT, true));
            Intent desktop = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            check(!desktop.getBooleanExtra(BatteryManager.EXTRA_PRESENT, true)
                && desktop.getIntExtra(BatteryManager.EXTRA_STATUS, 0) == BatteryManager.BATTERY_STATUS_FULL, "no-battery desktop policy");
            fixture(new int[]{255, 255, 255, 255});
            await(() -> manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) == Integer.MIN_VALUE
                && !battery(context).hasExtra(BatteryManager.EXTRA_PRESENT));
            Intent unknown = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            check(!unknown.hasExtra(BatteryManager.EXTRA_PRESENT) && unknown.getIntExtra(BatteryManager.EXTRA_LEVEL, 0) == -1,
                "unknown Windows values must not be reported as no battery");
            fixture(new int[]{1, 255, 50, 0});
            await(() -> manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY) == 50
                && battery(context).getIntExtra(BatteryManager.EXTRA_STATUS, 0) == BatteryManager.BATTERY_STATUS_UNKNOWN);
            check(manager.getIntProperty(BatteryManager.BATTERY_PROPERTY_STATUS) == BatteryManager.BATTERY_STATUS_UNKNOWN
                && !battery(context).hasExtra(BatteryManager.EXTRA_PRESENT), "partial telemetry must preserve unknown charging status");
            for (int index = 1; index < sticky.size(); index++) check(!sticky.get(index), "live updates must not be initial sticky broadcasts");
            for (String action : new String[]{Intent.ACTION_BATTERY_CHANGED, Intent.ACTION_BATTERY_LOW, Intent.ACTION_BATTERY_OKAY,
                    Intent.ACTION_POWER_CONNECTED, Intent.ACTION_POWER_DISCONNECTED, BatteryManager.ACTION_CHARGING,
                    BatteryManager.ACTION_DISCHARGING, PowerManager.ACTION_POWER_SAVE_MODE_CHANGED}) {
                try { context.sendBroadcast(new Intent(action)); throw new AssertionError("untrusted system broadcast accepted: " + action); }
                catch (SecurityException expected) { }
            }
            System.out.println("PASS framework battery state, sticky delivery, transitions, unknown telemetry, protected actions");
        } finally { context.unregisterReceiver(receiver); fixture(initialHost); }
        return 42;
    }
}
