package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.os.IBinder;
import android.os.IInterface;
import android.os.BatteryManager;
import android.os.PowerManager;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;

/** Local broadcast routing through the selected image's ActivityThread dispatchers. */
final class ArtBroadcasts {
    private final ArtPackage apk;
    private final int user;
    private final Map<IBinder, ArrayList<Registration>> receivers = new LinkedHashMap<>();
    private final Map<String, Intent> systemSticky = new LinkedHashMap<>();
    private volatile ArtApplication application;
    private static final class Registration {
        final IInterface receiver;
        final IntentFilter filter;
        final String permission;
        boolean initialPending = true;
        Registration(IInterface receiver, IntentFilter filter, String permission) {
            this.receiver = receiver; this.filter = new IntentFilter(filter); this.permission = permission;
        }
    }
    ArtBroadcasts(ArtPackage apk, int user) { this.apk = apk; this.user = user; }
    synchronized void attach(ArtApplication application) {
        this.application = application;
        if (application == null) return;
        for (ArrayList<Registration> filters : receivers.values()) for (Registration registration : filters)
            if (registration.initialPending) deliverInitial(registration);
    }

    private ArrayList<Intent> matchingSticky(IntentFilter filter) {
        ArrayList<Intent> result = new ArrayList<>();
        for (Intent intent : systemSticky.values()) if (filter.match(intent.getAction(), null, intent.getScheme(),
                    intent.getData(), intent.getCategories(), "StoneSticky") >= 0) result.add(ArtWifi.copyBroadcast(intent));
        return result;
    }
    private void deliverInitial(Registration registration) {
        try {
            for (Intent intent : matchingSticky(registration.filter))
                application.scheduleRegisteredReceiver(registration.receiver, intent, 1000, "android", true);
            registration.initialPending = false;
        } catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot deliver initial system broadcast", error); }
    }

    void publishSystem(Intent intent, boolean sticky) throws ReflectiveOperationException {
        if (!protectedPowerAction(intent.getAction())) throw new SecurityException("Unknown system broadcast");
        synchronized (this) { if (sticky) systemSticky.put(intent.getAction(), ArtWifi.copyBroadcast(intent)); }
        if (application != null) dispatch(intent, null, 1000, "android");
    }

    private static boolean protectedPowerAction(String action) {
        return Intent.ACTION_BATTERY_CHANGED.equals(action) || Intent.ACTION_BATTERY_LOW.equals(action)
            || Intent.ACTION_BATTERY_OKAY.equals(action) || Intent.ACTION_POWER_CONNECTED.equals(action)
            || Intent.ACTION_POWER_DISCONNECTED.equals(action) || BatteryManager.ACTION_CHARGING.equals(action)
            || BatteryManager.ACTION_DISCHARGING.equals(action) || PowerManager.ACTION_POWER_SAVE_MODE_CHANGED.equals(action)
            || android.net.wifi.WifiManager.WIFI_STATE_CHANGED_ACTION.equals(action)
            || android.net.wifi.WifiManager.NETWORK_STATE_CHANGED_ACTION.equals(action);
    }

    synchronized Object register(Object[] args) {
        if (!apk.applicationInfo.packageName.equals(args[1]) || (Integer) args[7] != user)
            throw new SecurityException("Only the local application and user can register receivers");
        if (args[4] != null) {
            IInterface receiver = (IInterface) args[4];
            ArrayList<Registration> filters = receivers.get(receiver.asBinder());
            if (filters == null) { filters = new ArrayList<>(); receivers.put(receiver.asBinder(), filters); }
            Registration registration = new Registration(receiver, (IntentFilter) args[5], (String) args[6]);
            filters.add(registration);
            if (application != null) deliverInitial(registration);
        }
        ArrayList<Intent> sticky = matchingSticky((IntentFilter) args[5]);
        return sticky.isEmpty() ? null : sticky.get(0);
    }

    synchronized void unregister(IInterface receiver) { receivers.remove(receiver.asBinder()); }

    int broadcast(Object[] args) throws ReflectiveOperationException {
        if ((Integer) args[15] != user) throw new SecurityException("Broadcast is outside the current user");
        if ((Boolean) args[13] || (Boolean) args[14] || args[8] != null || args[9] != null || args[10] != null)
            throw new UnsupportedOperationException("Ordered, sticky and permission-filtered broadcasts are not connected yet");
        String action = ((Intent) args[2]).getAction();
        if (Intent.ACTION_PACKAGE_ADDED.equals(action) || Intent.ACTION_PACKAGE_REMOVED.equals(action)
                || Intent.ACTION_PACKAGE_REPLACED.equals(action) || Intent.ACTION_PACKAGE_FULLY_REMOVED.equals(action)
                || Intent.ACTION_MY_PACKAGE_REPLACED.equals(action) || Intent.ACTION_UID_REMOVED.equals(action)
                || protectedPowerAction(action))
            throw new SecurityException("Applications cannot send protected system broadcasts");
        dispatch((Intent) args[2], (String) args[3], apk.applicationInfo.uid, apk.applicationInfo.packageName);
        return 0;
    }

    void dispatch(Intent request, String resolvedType, int sendingUid, String sendingPackage) throws ReflectiveOperationException {
        ArtApplication target = application;
        if (target == null) throw new IllegalStateException("Application broadcast dispatcher is not ready");
        Intent intent = new Intent(request);
        String owner = apk.applicationInfo.packageName;
        if (intent.getPackage() != null && !owner.equals(intent.getPackage())) return;
        ComponentName component = intent.getComponent();
        if (component != null) {
            if (!owner.equals(component.getPackageName())) return;
            ActivityInfo receiver = apk.receiver(component);
            if (receiver != null) target.scheduleReceiver(receiver, intent, sendingUid, sendingPackage);
            return;
        }
        ArrayList<ArrayList<Registration>> snapshot = new ArrayList<>();
        synchronized (this) { for (ArrayList<Registration> filters : receivers.values()) snapshot.add(new ArrayList<>(filters)); }
        for (ArrayList<Registration> filters : snapshot) for (Registration receiver : filters) {
            if (receiver.permission != null && sendingUid != 1000) continue;
            if (receiver.filter.match(intent.getAction(), resolvedType, intent.getScheme(), intent.getData(),
                    intent.getCategories(), "StoneBroadcast") >= 0) {
                target.scheduleRegisteredReceiver(receiver.receiver, ArtWifi.copyBroadcast(intent), sendingUid, sendingPackage);
                break; // One delivery per receiver, even when several of its filters match.
            }
        }
    }
}
