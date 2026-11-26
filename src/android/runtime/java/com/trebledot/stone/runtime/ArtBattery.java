package com.trebledot.stone.runtime;

import android.content.Intent;
import android.content.res.Resources;
import android.os.BatteryManager;
import android.os.PowerManager;
import android.util.Log;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Cached Windows power status. Native polling never runs on an APK's UI thread. */
final class ArtBattery {
    private static final int UNKNOWN = 255;
    private final ArtBroadcasts broadcasts;
    private final Method setProperty = Class.forName("android.os.BatteryProperty").getMethod("setLong", long.class);
    private final ScheduledExecutorService monitor = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "Stone battery"); thread.setDaemon(true); return thread;
    });
    private volatile State state;
    private final int warning, closeWarning;
    private boolean sentLow;

    ArtBattery(ArtBroadcasts broadcasts) throws ReflectiveOperationException {
        this.broadcasts = broadcasts;
        warning = Math.min(100, Math.max(config("config_criticalBatteryWarningLevel", 5), config("config_lowBatteryWarningLevel", 15)));
        closeWarning = Math.min(100, warning + Math.max(0, config("config_lowBatteryCloseWarningBump", 5)));
        try { state = new State(queryHostState(), null, warning, closeWarning); }
        catch (Exception error) { Log.w("StoneBattery", "Cannot read initial Windows power status", error); state = new State(new int[]{255, 255, 255, 255}, null, warning, closeWarning); }
        sentLow = Boolean.TRUE.equals(state.low);
        broadcasts.publishSystem(state.intent(), true);
        monitor.scheduleWithFixedDelay(() -> {
            try { publish(queryHostState()); }
            catch (Exception error) { Log.w("StoneBattery", "Cannot refresh Windows power status", error); }
        }, 1, 1, TimeUnit.SECONDS);
    }

    private static native int[] queryHostState();

    private static int config(String name, int fallback) {
        Resources resources = Resources.getSystem();
        int id = resources.getIdentifier(name, "integer", "android");
        return id == 0 ? fallback : resources.getInteger(id);
    }

    private synchronized void publish(int[] raw) throws ReflectiveOperationException {
        State previous = state;
        State next = new State(raw, previous.low, warning, closeWarning);
        if (Arrays.equals(previous.raw, next.raw)) return;
        Intent changed = next.intent();
        state = next;
        broadcasts.publishSystem(changed, true);
        if (previous.ac != UNKNOWN && next.ac != UNKNOWN && previous.ac != next.ac)
            broadcast(next.ac == 1 ? Intent.ACTION_POWER_CONNECTED : Intent.ACTION_POWER_DISCONNECTED);
        if (previous.status != BatteryManager.BATTERY_STATUS_UNKNOWN && next.status != BatteryManager.BATTERY_STATUS_UNKNOWN
                && previous.charging() != next.charging())
            broadcast(next.charging() ? BatteryManager.ACTION_CHARGING : BatteryManager.ACTION_DISCHARGING);
        if (Boolean.TRUE.equals(next.low) && !Boolean.TRUE.equals(previous.low)) {
            sentLow = true; broadcast(Intent.ACTION_BATTERY_LOW);
        } else if (sentLow && next.level >= closeWarning) {
            sentLow = false; broadcast(Intent.ACTION_BATTERY_OKAY);
        }
        if (previous.saver != UNKNOWN && next.saver != UNKNOWN && previous.saver != next.saver)
            broadcast(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED);
    }

    private void broadcast(String action) throws ReflectiveOperationException {
        broadcasts.publishSystem(new Intent(action).addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY), false);
    }

    Object stats(Object instance, Method method, Object[] args) {
        switch (method.getName()) {
            case "isCharging": return state.charging();
            case "computeChargeTimeRemaining": return -1L; // No Windows charging-time estimate is available.
            default: throw new UnsupportedOperationException("ART battery stats: " + method.getName());
        }
    }
    Object properties(Object instance, Method method, Object[] args) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "getProperty": {
                State current = state;
                long value;
                switch ((Integer) args[0]) {
                    case BatteryManager.BATTERY_PROPERTY_CAPACITY:
                        if (current.level < 0) return -1;
                        value = current.level; break;
                    case BatteryManager.BATTERY_PROPERTY_STATUS: value = current.status; break;
                    default: return -1;
                }
                setProperty.invoke(args[1], value); return 0;
            }
            case "getPropertyString": return null;
            case "scheduleUpdate": return null; // The monitor owns refreshes.
            default: throw new UnsupportedOperationException("ART battery properties: " + method.getName());
        }
    }

    private static final class State {
        final int[] raw;
        final int ac, level, saver, status;
        final boolean absent;
        final Boolean low;
        State(int[] values, Boolean previousLow, int warning, int closeWarning) {
            if (values == null || values.length != 4) throw new IllegalArgumentException("Invalid Windows power status");
            for (int value : values) if (value < 0 || value > 255) throw new IllegalArgumentException("Invalid Windows power field");
            raw = values.clone(); ac = raw[0] <= 1 ? raw[0] : UNKNOWN; saver = raw[3] <= 1 ? raw[3] : UNKNOWN;
            absent = raw[1] != UNKNOWN && (raw[1] & 128) != 0;
            // A desktop without a battery is an always-supplied Android device.
            // Keep EXTRA_PRESENT=false while allowing charge/battery constraints.
            level = absent ? 100 : raw[2] <= 100 ? raw[2] : -1;
            status = raw[1] == UNKNOWN ? BatteryManager.BATTERY_STATUS_UNKNOWN
                : absent && ac == 1 || level == 100 && ac == 1 ? BatteryManager.BATTERY_STATUS_FULL
                : raw[1] != UNKNOWN && (raw[1] & 8) != 0 ? BatteryManager.BATTERY_STATUS_CHARGING
                : ac == 0 && !absent ? BatteryManager.BATTERY_STATUS_DISCHARGING
                : ac == 1 && !absent ? BatteryManager.BATTERY_STATUS_NOT_CHARGING : BatteryManager.BATTERY_STATUS_UNKNOWN;
            if (absent || ac == 1) low = false;
            else if (level < 0 || ac == UNKNOWN || status == BatteryManager.BATTERY_STATUS_UNKNOWN) low = null;
            else low = Boolean.TRUE.equals(previousLow) ? level < closeWarning : level <= warning;
        }
        boolean charging() { return status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL; }
        Intent intent() {
            Intent result = new Intent(Intent.ACTION_BATTERY_CHANGED).addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY)
                .putExtra(BatteryManager.EXTRA_STATUS, status).putExtra(BatteryManager.EXTRA_HEALTH, BatteryManager.BATTERY_HEALTH_UNKNOWN)
                .putExtra(BatteryManager.EXTRA_LEVEL, level).putExtra(BatteryManager.EXTRA_SCALE, 100)
                .putExtra(BatteryManager.EXTRA_PLUGGED, ac == 1 ? BatteryManager.BATTERY_PLUGGED_AC : 0);
            if (raw[1] != UNKNOWN) result.putExtra(BatteryManager.EXTRA_PRESENT, !absent);
            if (low != null) result.putExtra(BatteryManager.EXTRA_BATTERY_LOW, low.booleanValue());
            return result;
        }
    }
}
