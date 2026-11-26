package com.trebledot.stone.probe;

import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.Context;
import android.os.Parcel;
import android.util.Base64;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Properties;

/** Public channel queries and real persisted state without requiring Windows registration. */
final class NotificationProbe {
    static void run(Context context) throws Exception {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (!java.util.Arrays.asList(context.getPackageManager().getPackageInfo(context.getPackageName(), 0).requestedPermissions)
                .contains("android.permission.POST_NOTIFICATIONS")) {
            int request = (Integer) Context.class.getMethod("getPermissionRequestState", String.class).invoke(context, "android.permission.POST_NOTIFICATIONS");
            if (request != 2 || context.checkSelfPermission("android.permission.POST_NOTIFICATIONS") == 0)
                throw new AssertionError("Undeclared notification permission was requestable or granted");
        }
        String persistent = "stone-cit-persist", blocked = "stone-cit-blocked";
        File marker = new File(context.getFilesDir(), "notification-channel-restart.txt");
        boolean restarted = marker.isFile();
        if (restarted) {
            NotificationChannel previous = manager.getNotificationChannel(persistent);
            if (previous == null || previous.getImportance() != NotificationManager.IMPORTANCE_LOW
                    || !"persisted channel".equals(previous.getDescription())) throw new AssertionError("Notification channel did not survive process restart");
        }
        NotificationChannel channel = new NotificationChannel(persistent, "Persisted", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("persisted channel"); manager.createNotificationChannel(channel);
        NotificationChannel returned = manager.getNotificationChannel(persistent);
        if (returned == null) throw new AssertionError("Public notification channel query failed");
        returned.setImportance(NotificationManager.IMPORTANCE_HIGH);
        NotificationChannel higher = new NotificationChannel(persistent, "Renamed", NotificationManager.IMPORTANCE_HIGH);
        higher.setDescription("persisted channel"); manager.createNotificationChannel(higher);
        if (manager.getNotificationChannel(persistent).getImportance() != NotificationManager.IMPORTANCE_LOW)
            throw new AssertionError("Channel recreation or returned-object mutation increased its importance");
        manager.createNotificationChannel(new NotificationChannel(blocked, "Blocked", NotificationManager.IMPORTANCE_NONE));
        manager.deleteNotificationChannel(blocked);
        if (manager.getNotificationChannel(blocked) != null) throw new AssertionError("Deleted channel was still returned");
        manager.createNotificationChannel(new NotificationChannel(blocked, "Restored", NotificationManager.IMPORTANCE_HIGH));
        if (manager.getNotificationChannel(blocked).getImportance() != NotificationManager.IMPORTANCE_NONE)
            throw new AssertionError("Deleted channel recreation bypassed its blocked state");
        File state = new File(context.getApplicationInfo().dataDir, ".stone-notification-channels.properties");
        String key = "channel." + Base64.encodeToString(persistent.getBytes(java.nio.charset.StandardCharsets.UTF_8), Base64.NO_WRAP);
        CommitProbe.await(() -> {
            if (!state.isFile()) return false;
            Properties saved = new Properties(); try (FileInputStream input = new FileInputStream(state)) { saved.load(input); }
            String encoded = saved.getProperty(key); if (encoded == null) return false;
            byte[] bytes = Base64.decode(encoded, Base64.DEFAULT); Parcel parcel = Parcel.obtain();
            try { parcel.unmarshall(bytes, 0, bytes.length); parcel.setDataPosition(0);
                NotificationChannel persisted = NotificationChannel.CREATOR.createFromParcel(parcel);
                return persisted.getImportance() == NotificationManager.IMPORTANCE_LOW && "persisted channel".equals(persisted.getDescription()); }
            finally { parcel.recycle(); }
        });
        try (FileOutputStream output = new FileOutputStream(marker)) { output.write(1); output.getFD().sync(); }
        System.out.println("ART notification public channel lookup, object isolation, importance protection, delete/recreate and persisted channel state passed. Restart=" + restarted);
    }
}
