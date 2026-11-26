package com.trebledot.stone.probe;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ServiceInfo;
import android.os.IBinder;
import java.lang.reflect.Field;

/** Exercises actual image Service dispatch using the original Java-only CIT components. */
final class ServiceProbe {
    private static final String NAME = "com.trebledot.stone.cit.components.ComponentService";
    private static Class<?> type;
    private static int count(String name) throws Exception {
        Field field = type.getDeclaredField(name); field.setAccessible(true); return field.getInt(null);
    }
    private static void check(boolean valid, String message) { if (!valid) throw new AssertionError(message); }
    static void process(Context context, int importance) throws Exception {
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        // The image ActivityManager rate-limits this public query. Let its cache expire.
        await(() -> {
            java.util.List<ActivityManager.RunningAppProcessInfo> values = manager.getRunningAppProcesses();
            return values != null && values.size() == 1 && values.get(0).importance == importance;
        });
        java.util.List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        check(processes != null && processes.size() == 1, "local process enumeration");
        ActivityManager.RunningAppProcessInfo info = processes.get(0);
        check(info.pid == android.os.Process.myPid() && info.uid == context.getApplicationInfo().uid
            && info.processName.equals(context.getApplicationInfo().processName)
            && java.util.Arrays.equals(info.pkgList, new String[]{context.getPackageName()}), "process identity");
        check(info.importance == importance, "process importance: " + info.importance + " expected " + importance);
    }
    private interface Condition { boolean done() throws Exception; }
    private static void await(Condition condition) throws Exception {
        long limit = android.os.SystemClock.uptimeMillis() + 10000;
        do { BroadcastProbe.drain(); if (condition.done()) return; Thread.sleep(2); }
        while (android.os.SystemClock.uptimeMillis() < limit);
        throw new AssertionError("Service callback timed out");
    }
    private static final class Connection implements ServiceConnection {
        IBinder binder;
        int connected, empty, dead;
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            check(android.os.Looper.myLooper() == android.os.Looper.getMainLooper(), "service connection thread");
            this.binder = binder; connected++;
        }
        @Override public void onServiceDisconnected(ComponentName name) { dead++; }
        @Override public void onBindingDied(ComponentName name) { dead++; }
        @Override public void onNullBinding(ComponentName name) { empty++; }
    }
    private static Service instance(IBinder binder) throws Exception {
        java.lang.reflect.Method method = binder.getClass().getDeclaredMethod("service");
        method.setAccessible(true); return (Service) method.invoke(binder);
    }
    @SuppressWarnings("deprecation")
    static void run(Context context) throws Exception {
        type = Class.forName(NAME);
        Intent request = new Intent().setComponent(new ComponentName(context.getPackageName(), NAME));
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        check(context.startService(new Intent(request).setClassName(context, NAME + "$Disabled")) == null, "disabled service ran");
        check(context.startService(new Intent(request).setClassName(context, NAME + "$Missing")) == null, "unknown service resolved");
        for (String suffix : new String[]{"$Remote", "$Isolated"}) {
            try { context.startService(new Intent(request).setClassName(context, NAME + suffix)); throw new AssertionError("unsupported service process accepted"); }
            catch (UnsupportedOperationException expected) { }
        }
        Connection inactive = new Connection();
        check(!context.bindService(request, inactive, 0), "binding created a service without AUTO_CREATE");
        context.unbindService(inactive);
        check(context.startService(request) != null && context.startService(request) != null, "service start resolution");
        check(count("creates") == 0 && count("starts") == 0, "service lifecycle ran inline");
        Connection first = new Connection(), second = new Connection();
        check(context.bindService(request, first, Context.BIND_AUTO_CREATE)
            && context.bindService(request, second, Context.BindServiceFlags.of(Context.BIND_AUTO_CREATE)), "service bind request");
        await(() -> second.connected == 1 && first.connected == 1);
        check(first.binder == second.binder && count("creates") == 1 && count("starts") == 2 && count("binds") == 1, "service sharing and start delivery");
        Service service = instance(first.binder);
        process(context, ActivityManager.RunningAppProcessInfo.IMPORTANCE_SERVICE);
        check(service.getApplication() == context.getApplicationContext(), "Service did not receive the real Application");
        check(!service.stopSelfResult(count("firstStart")), "old start ID stopped service");
        Notification notification = new Notification.Builder(context, "stone-cit-service")
            .setSmallIcon(android.R.drawable.ic_dialog_info).setContentTitle("Stone service probe").build();
        android.app.NotificationManager notifications = context.getSystemService(android.app.NotificationManager.class);
        notifications.createNotificationChannel(new android.app.NotificationChannel("stone-cit-service", "Service", android.app.NotificationManager.IMPORTANCE_LOW));
        try {
            service.startForeground(912, new Notification.Builder(context, "stone-cit-missing").setSmallIcon(android.R.drawable.ic_dialog_info).build());
            throw new AssertionError("missing foreground channel accepted");
        } catch (IllegalArgumentException expected) { }
        try {
            service.startForeground(912, new Notification.Builder(context, "stone-cit-service").build());
            throw new AssertionError("foreground notification without small icon accepted");
        } catch (IllegalArgumentException expected) { }
        check(service.getForegroundServiceType() == 0 && !manager.getRunningServices(1).get(0).foreground, "invalid notification changed foreground state");
        service.startForeground(912, notification);
        check(notifications.areNotificationsEnabled() || notifications.getActiveNotifications().length == 0,
            "foreground notification bypassed denied notification permission");
        process(context, ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND_SERVICE);
        check(service.getForegroundServiceType() == ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC, "manifest foreground type");
        check(manager.getRunningServices(1).size() == 1 && manager.getRunningServices(1).get(0).foreground
            && manager.getRunningServices(1).get(0).started, "foreground service state");
        try { service.startForeground(912, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK); throw new AssertionError("undeclared foreground type accepted"); }
        catch (IllegalArgumentException expected) { }
        service.stopForeground(Service.STOP_FOREGROUND_REMOVE);
        process(context, ActivityManager.RunningAppProcessInfo.IMPORTANCE_SERVICE);
        check(service.getForegroundServiceType() == 0 && !manager.getRunningServices(1).get(0).foreground, "stop foreground state");
        context.unbindService(first); context.unbindService(second);
        await(() -> count("unbinds") == 1);
        check(count("destroys") == 0, "unbind destroyed a started service");
        Connection again = new Connection();
        check(context.bindService(request, again, Context.BIND_AUTO_CREATE), "rebind request");
        await(() -> again.connected == 1);
        check(again.binder == first.binder && count("rebinds") == 1 && count("binds") == 1, "onRebind did not reuse binder");
        check(service.stopSelfResult(count("latestStart")), "latest start ID did not stop service");
        check(count("destroys") == 0, "stopSelf destroyed an auto-created binding");
        context.unbindService(again);
        await(() -> count("destroys") == 1);
        check(!service.stopSelfResult(count("latestStart")), "destroyed service token stopped a later instance");

        // An unbind queued before onBind must not destroy the instance if a new client arrives.
        Intent empty = new Intent(request).setAction("nullBinding").putExtra("null", true);
        Connection cancelled = new Connection(), nullResult = new Connection();
        check(context.bindService(empty, cancelled, Context.BIND_AUTO_CREATE), "queued null binding");
        context.unbindService(cancelled);
        check(context.bindService(empty, nullResult, Context.BIND_AUTO_CREATE), "replacement null binding");
        await(() -> nullResult.empty == 1);
        check(cancelled.connected == 0 && cancelled.empty == 0, "cancelled binding was delivered");
        context.unbindService(nullResult);
        await(() -> count("destroys") == 2);
        check(count("creates") == 2 && count("binds") == 2 && count("unbinds") == 4, "null binder and queued rebind lifecycle");

        check(context.startService(request) != null && context.stopService(request), "queued start then stop");
        await(() -> count("destroys") == 3);
        check(count("starts") == 3 && !context.stopService(request), "explicit stop lifecycle");
        check(context.startService(request) != null, "started non-auto binding fixture");
        Connection nonAuto = new Connection();
        check(context.bindService(request, nonAuto, 0), "bind existing service without AUTO_CREATE");
        await(() -> nonAuto.connected == 1);
        check(context.stopService(request), "stop bound non-auto service");
        await(() -> count("destroys") == 4 && nonAuto.dead > 0);
        context.unbindService(nonAuto);
        check(manager.getRunningServices(10).isEmpty(), "destroyed services remain registered");

        Intent freshBinding = new Intent(request).setAction("freshBinding").putExtra("no-rebind", true);
        check(context.startService(request) != null, "no-rebind fixture start");
        Connection fresh = new Connection();
        check(context.bindService(freshBinding, fresh, Context.BIND_AUTO_CREATE), "first no-rebind connection");
        await(() -> fresh.connected == 1);
        int previousBinds = count("binds"), previousUnbinds = count("unbinds"), previousRebinds = count("rebinds");
        context.unbindService(fresh);
        await(() -> count("unbinds") > previousUnbinds);
        Connection reboundFresh = new Connection();
        check(context.bindService(freshBinding, reboundFresh, Context.BIND_AUTO_CREATE), "second no-rebind connection");
        await(() -> reboundFresh.connected == 1);
        check(count("binds") == previousBinds + 1 && count("rebinds") == previousRebinds, "onUnbind(false) did not request a fresh onBind");
        check(context.stopService(request), "no-rebind fixture stop");
        context.unbindService(reboundFresh);
        await(() -> count("destroys") == 5);

        check(context.startForegroundService(request) != null, "foreground start request");
        await(() -> count("starts") == 6);
        check(context.stopService(request), "foreground start cleanup");
        await(() -> count("destroys") == 6);
        Throwable[] workerError = new Throwable[1];
        Thread worker = new Thread(() -> {
            try { check(context.startService(request) != null, "worker service start"); }
            catch (Throwable error) { workerError[0] = error; }
        }, "Stone service caller probe");
        worker.start(); worker.join(5000);
        check(!worker.isAlive() && workerError[0] == null, "worker start did not return independently of the main Looper");
        await(() -> count("starts") == 7);
        check(context.stopService(request), "worker service cleanup");
        await(() -> count("destroys") == 7);
        Field onMain = type.getDeclaredField("allOnMain"); onMain.setAccessible(true);
        check(onMain.getBoolean(null), "APK Service lifecycle did not run on main");
        process(context, ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED);
        System.out.println("ART delivered actual APK Service creation, starts, shared/null binding, rebind, stop IDs, foreground state and destruction on the image main thread.");
    }
}
