package com.trebledot.stone.probe;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.AssetManager;
import android.service.notification.StatusBarNotification;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.io.FileOutputStream;
import java.util.Map;

/** Independent foreground services sharing notification IDs. */
public final class ForegroundSample {
    private static Context context;
    private static NotificationManager notifications;
    private static Map<?, Service> services;
    private static String first;
    private static void check(boolean valid, String message) { if (!valid) throw new AssertionError(message); }
    private static void write(String name, String value) throws Exception {
        try (FileOutputStream output = new FileOutputStream("/data/" + name)) { output.write(value.getBytes("UTF-8")); output.getFD().sync(); }
    }
    private static Notification active(int id) {
        for (StatusBarNotification value : notifications.getActiveNotifications()) if (value.getId() == id && value.getTag() == null) return value.getNotification();
        return null;
    }
    private static Notification note(String title) {
        return new Notification.Builder(context, "stone-foreground-integration").setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentTitle(title).setContentText("Foreground notification 状态").setOnlyAlertOnce(true).build();
    }
    private static Service find(String name) { for (Service service : services.values()) if (service.getClass().getName().equals(name)) return service; return null; }
    private static Service start(String name, int id, boolean allowed) throws Exception {
        Intent intent = new Intent().setComponent(new ComponentName(context.getPackageName(), name));
        check(context.startForegroundService(intent) != null, "service did not resolve");
        CommitProbe.await(() -> find(name) != null);
        Service service = find(name);
        service.startForeground(id, note("FGS fixture " + id));
        if (allowed) CommitProbe.await(() -> active(id) != null && NotificationHistoryProbe.history().length > 0);
        return service;
    }
    private static void stop(Service service) throws Exception {
        check(context.stopService(new Intent().setComponent(new ComponentName(context.getPackageName(), service.getClass().getName()))), "stopService failed");
        CommitProbe.await(() -> !services.containsValue(service));
    }
    private static void empty() throws Exception { CommitProbe.await(() -> notifications.getActiveNotifications().length == 0 && NotificationHistoryProbe.history().length == 0); }
    private static void foreground(int id, boolean expected) {
        Notification value = active(id);
        check(value != null && ((value.flags & Notification.FLAG_FOREGROUND_SERVICE) != 0) == expected, "notification foreground flag: " + id);
    }
    private static void quiet() throws Exception { long end = android.os.SystemClock.uptimeMillis() + 500;
        while (android.os.SystemClock.uptimeMillis() < end) { BroadcastProbe.drain(); Thread.sleep(10); } }

    @SuppressWarnings("deprecation")
    public static int run(AssetManager assets, String expectation) throws Exception {
        boolean allowed = "allowed".equals(expectation);
        check(allowed || "denied".equals(expectation), "invalid permission expectation");
        java.lang.reflect.Method initialize = Class.forName("com.trebledot.stone.runtime.ArtConfiguration").getDeclaredMethod("initialize");
        initialize.setAccessible(true); initialize.invoke(null);
        ArtPackage apk = ArtPackage.openApplication(assets, "/data/application.apk");
        check("com.trebledot.stone.foreground.fixture".equals(apk.applicationInfo.packageName), "unexpected foreground sample");
        ArtApplication running = ArtApplication.start(apk); context = running.context;
        notifications = context.getSystemService(NotificationManager.class);
        if (allowed) CommitProbe.await(notifications::areNotificationsEnabled);
        else check(!notifications.areNotificationsEnabled(), "denied test unexpectedly granted");
        notifications.createNotificationChannel(new NotificationChannel("stone-foreground-integration", "Foreground integration", NotificationManager.IMPORTANCE_LOW));
        Class<?> thread = Class.forName("android.app.ActivityThread"); Object current = thread.getMethod("currentActivityThread").invoke(null);
        java.lang.reflect.Field field = thread.getDeclaredField("mServices"); field.setAccessible(true);
        @SuppressWarnings("unchecked") Map<?, Service> live = (Map<?, Service>) field.get(current); services = live;
        first = "com.trebledot.stone.probe.ForegroundFixture$First";
        int id = 5100;
        Service service = start(first, id, allowed);
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        check(manager.getRunningServices(10).get(0).foreground, "service did not enter foreground state");
        if (!allowed) {
            check(notifications.getActiveNotifications().length == 0 && NotificationHistoryProbe.history().length == 0, "denied foreground notification was delivered");
            stop(service); empty();
            System.out.println("Fixture service remained foreground with notifications denied, then stopped without a Windows notification.");
            return 42;
        }
        write("foreground-original.xml", "<history>" + String.join("", NotificationHistoryProbe.history()) + "</history>");
        foreground(id, true);
        try {
            service.startForeground(5190, new Notification.Builder(context, "missing-foreground-channel").setSmallIcon(android.R.drawable.ic_dialog_info).build());
            throw new AssertionError("invalid foreground replacement accepted");
        } catch (IllegalArgumentException expected) { }
        foreground(id, true); check(active(5190) == null && service.getForegroundServiceType() != 0, "invalid replacement damaged existing service notification");
        notifications.cancel(id); quiet(); foreground(id, true);
        notifications.notify("ordinary", id, note("FGS tagged ordinary"));
        notifications.cancelAll(); quiet(); foreground(id, true);
        check(notifications.getActiveNotifications().length == 1 && NotificationHistoryProbe.history().length == 1, "cancelAll removed an owned foreground notification or retained ordinary notice");
        Notification update = note("FGS updated"); update.flags |= Notification.FLAG_AUTO_CANCEL;
        boolean[] clicked = {false};
        BroadcastReceiver receiver = new BroadcastReceiver() { @Override public void onReceive(Context c, Intent intent) { clicked[0] = true; } };
        context.registerReceiver(receiver, new IntentFilter("stone.foreground.CONTENT"), Context.RECEIVER_NOT_EXPORTED);
        try {
            update.contentIntent = PendingIntent.getBroadcast(context, 7901, new Intent("stone.foreground.CONTENT").setPackage(context.getPackageName()), PendingIntent.FLAG_IMMUTABLE);
            notifications.notify(id, update);
            CommitProbe.await(() -> {
                for (String xml : NotificationHistoryProbe.history()) if (xml.contains("FGS updated")) return true;
                return false;
            });
            foreground(id, true);
            String xml = NotificationHistoryProbe.history()[0];
            org.xmlpull.v1.XmlPullParser parser = android.util.Xml.newPullParser(); parser.setInput(new java.io.StringReader(xml));
            String url = null;
            while (parser.next() != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) if (parser.getEventType() == org.xmlpull.v1.XmlPullParser.START_TAG && "toast".equals(parser.getName())) url = parser.getAttributeValue(null, "launch");
            check(url != null, "foreground content activation missing");
            write("foreground-content.url", url); write("foreground-content-ready", "ready");
            CommitProbe.await(() -> clicked[0]); quiet();
            foreground(id, true); check(NotificationHistoryProbe.history().length == 1, "auto-cancel removed foreground notification");
            write("foreground-content-observed", "passed");
        } finally { context.unregisterReceiver(receiver); }
        policy(service, id);
        service.startForeground(5101, note("FGS replacement"));
        CommitProbe.await(() -> active(id) == null && active(5101) != null && NotificationHistoryProbe.history().length == 1);
        service.stopForeground(Service.STOP_FOREGROUND_REMOVE); empty();
        check(!manager.getRunningServices(10).get(0).foreground && service.getForegroundServiceType() == 0, "stopForeground did not demote service");
        stop(service);
        service = start(first, id, true); service.stopForeground(false); foreground(id, false); stop(service); empty();
        service = start(first, id, true); service.stopForeground(Service.STOP_FOREGROUND_DETACH); foreground(id, false); stop(service);
        quiet(); check(active(id) != null && NotificationHistoryProbe.history().length == 1, "detached notification was removed at destruction");
        notifications.cancel(id); empty();
        service = start(first, id, true); stop(service); empty();
        Notification forged = note("Forged foreground"); forged.flags |= Notification.FLAG_FOREGROUND_SERVICE;
        notifications.notify(5199, forged); foreground(5199, false); notifications.cancel(5199); empty();
        shared();
        check(manager.getRunningServices(10).isEmpty(), "foreground fixture leaked services");
        System.out.println("Foreground Windows history, protected cancel/update/content, ID replacement, REMOVE, LEGACY, DETACH and destruction passed");
        return 42;
    }
    private static void policy(Service service, int id) throws Exception {
        ActivityManager manager = context.getSystemService(ActivityManager.class);
        write("notification-policy-ready", "ready");
        for (String name : new String[]{"global", "app"}) {
            CommitProbe.await(() -> !notifications.areNotificationsEnabled() && notifications.getActiveNotifications().length == 0 && NotificationHistoryProbe.history().length == 0);
            check(service.getForegroundServiceType() != 0 && manager.getRunningServices(10).get(0).foreground,
                "revoking notification permission stopped foreground state");
            write("notification-" + name + "-off-observed", "passed");
            CommitProbe.await(notifications::areNotificationsEnabled);
            service.startForeground(id, note("FGS permission restored " + name));
            CommitProbe.await(() -> active(id) != null && NotificationHistoryProbe.history().length == 1);
            foreground(id, true);
            write("notification-" + name + "-on-observed", "passed");
        }
        System.out.println("Real settings global/app switches hid foreground notifications without stopping the service, then allowed service updates again.");
    }
    private static void shared() throws Exception {
        String other = "com.trebledot.stone.probe.ForegroundFixture$Second";
        for (int mode : new int[]{Service.STOP_FOREGROUND_REMOVE, 0}) {
            Service one = start(first, 6000, true), two = start(other, 6000, true);
            one.stopForeground(mode); foreground(6000, true); stop(one); quiet(); foreground(6000, true);
            two.stopForeground(Service.STOP_FOREGROUND_DETACH); foreground(6000, false); stop(two);
            check(active(6000) != null, "shared detached notice was removed"); notifications.cancel(6000); empty();
        }
        Service one = start(first, 6001, true), two = start(other, 6001, true);
        one.startForeground(6002, note("FGS separate owner"));
        CommitProbe.await(() -> active(6001) != null && active(6002) != null && NotificationHistoryProbe.history().length == 2);
        stop(two); CommitProbe.await(() -> active(6001) == null); foreground(6002, true);
        stop(one); empty();
        System.out.println("Two real Android services shared an ID without cross-cancellation, detached it, and replaced it independently.");
    }
}
