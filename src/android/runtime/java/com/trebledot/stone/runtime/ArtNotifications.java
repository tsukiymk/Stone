package com.trebledot.stone.runtime;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.service.notification.StatusBarNotification;
import android.util.AtomicFile;
import android.util.Base64;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Per-application notification state and asynchronous Windows Toast delivery. */
final class ArtNotifications {
    private static native boolean initializeNative(String packageName) throws IOException;
    private static native boolean allowedNative() throws IOException;
    private static native String[] postNative(String key, String title, String text, String[] actions,
        int progress, int maximum, boolean indeterminate, boolean silent, boolean suppressPopup,String[] targets,String channel,int flags) throws IOException;
    private static native void removeNative(String key) throws IOException;
    private static native void removeAllNative() throws IOException;
    private static native long[] pollNative();
    private final ArtPackage apk;
    private final int user;
    final ArtToasts toasts = new ArtToasts();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Map<String, NotificationChannel> channels = new HashMap<>();
    private final java.util.Set<String> deleted = new java.util.HashSet<>();
    private final Map<String, Posted> active = new HashMap<>();
    private final Map<android.os.IBinder, ServiceNotification> services = new HashMap<>();
    private static final class ServiceNotification {
        final int id;
        boolean foreground = true;
        ServiceNotification(int id) { this.id = id; }
    }
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor(action -> {
        Thread thread = new Thread(action, "Stone notifications"); thread.setDaemon(true); return thread;
    });
    private volatile boolean enabled, consent;
    private boolean initialized, warned;
    private long retryAfter;
    private long hostGeneration;
    private String applicationName;
    private static final class Posted {
        final int id;
        final String tag, key;
        final Notification notification;
        final int originalFlags;
        final long time = System.currentTimeMillis();
        final List<PendingIntent> actions = new ArrayList<>();
        long ticket;
        String route = "";
        boolean replacing;
        Posted(String tag, int id, Notification notification) {
            this.tag = tag; this.id = id; this.notification = notification.clone();
            originalFlags = notification.flags & ~Notification.FLAG_FOREGROUND_SERVICE;
            key = (tag == null ? "-" : "+" + tag) + ":" + id;
        }
    }
    ArtNotifications(ArtPackage apk, int user) {
        this.apk = apk; this.user = user;
        applicationName = apk.applicationInfo.packageName;
        try { loadChannels(); } catch (Exception error) { Log.w("StoneNotifications", "Cannot read notification channels", error); }
        worker.execute(() -> { try { publishIdentity(); } catch (Exception error) { report(error); } });
        worker.scheduleWithFixedDelay(this::poll, 0, 250, TimeUnit.MILLISECONDS);
    }
    // New image frameworks implement areNotificationsEnabled through this permission query.
    boolean granted() { return enabled; }
    boolean consented() { return consent; }
    boolean requestable() { return ArtPermissions.requestable(apk, ArtPermissions.NOTIFICATIONS); }
    private NotificationChannel channel(Notification notification) {
        if (notification == null) throw new IllegalArgumentException("Missing notification");
        NotificationChannel channel = channels.get(notification.getChannelId());
        if (channel == null || deleted.contains(channel.getId())) throw new IllegalArgumentException("Notification channel does not exist");
        return channel;
    }
    private boolean foregroundId(int id) {
        for (ServiceNotification service : services.values()) if (service.foreground && service.id == id) return true;
        return false;
    }
    private boolean foreground(Posted posted) { return posted.tag == null && foregroundId(posted.id); }
    private void capacity(String key, NotificationChannel channel) {
        if (enabled && channel.getImportance() != NotificationManager.IMPORTANCE_NONE && !active.containsKey(key) && active.size() >= 256)
            throw new IllegalStateException("Too many active notifications");
    }
    private void enqueue(Posted posted, NotificationChannel channel) {
        if (!enabled || channel.getImportance() == NotificationManager.IMPORTANCE_NONE) return;
        capacity(posted.key, channel);
        posted.notification.flags = posted.originalFlags | (foreground(posted)
            ? Notification.FLAG_FOREGROUND_SERVICE | Notification.FLAG_NO_CLEAR : 0);
        posted.replacing = active.containsKey(posted.key);
        active.put(posted.key, posted); worker.execute(() -> publish(posted));
    }
    synchronized void startForeground(android.os.IBinder token, int id, Notification notification) {
        NotificationChannel channel = channel(notification);
        if (notification.getSmallIcon() == null) throw new IllegalArgumentException("Foreground notification requires a small icon");
        Posted posted = new Posted(null, id, notification);
        capacity(posted.key, channel);
        ServiceNotification previous = services.put(token, new ServiceNotification(id));
        if (previous != null && previous.id != id) release(previous.id, true);
        enqueue(posted, channel);
    }
    synchronized void stopForeground(android.os.IBinder token, int flags) {
        ServiceNotification service = services.get(token);
        if (service == null || !service.foreground) return;
        service.foreground = false;
        boolean remove = (flags & android.app.Service.STOP_FOREGROUND_REMOVE) != 0;
        if (remove || (flags & android.app.Service.STOP_FOREGROUND_DETACH) != 0) services.remove(token);
        release(service.id, remove);
    }
    synchronized void destroyService(android.os.IBinder token) {
        ServiceNotification service = services.remove(token);
        if (service != null) release(service.id, true);
    }
    private void release(int id, boolean remove) {
        if (foregroundId(id)) return; // Another live service still owns this untagged notification.
        String key = "-:" + id;
        if (remove) cancel(key);
        else {
            Posted posted = active.get(key);
            if (posted != null) posted.notification.flags = posted.originalFlags;
        }
    }
    void allow(Runnable completed) {
        worker.execute(() -> {
            try {
                if (!requestable()) throw new SecurityException("Notification permission was not declared");
                File policy = new File("/data/system/stone-notifications/policy.properties");
                File parent = policy.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create notification policy directory");
                try (java.io.RandomAccessFile gate = new java.io.RandomAccessFile(new File(parent, "policy.lock"), "rw");
                        java.nio.channels.FileLock lock = gate.getChannel().lock(0, 1, false)) {
                    Properties state = new Properties();
                    if (policy.isFile()) {
                        if (policy.length() > 1024 * 1024) throw new IOException("Notification policy is too large");
                        try (FileInputStream input = new FileInputStream(policy)) { state.load(input); }
                        if (!"1".equals(state.getProperty("format"))) throw new IOException("Invalid notification policy");
                    }
                    state.setProperty("format", "1");
                    if (!state.containsKey("global")) state.setProperty("global", "true");
                    state.setProperty("package." + apk.applicationInfo.packageName, "true");
                    AtomicFile file = new AtomicFile(policy); FileOutputStream output = file.startWrite();
                    try { state.store(output, "Stone notification permissions"); file.finishWrite(output); }
                    catch (Exception error) { file.failWrite(output); throw error; }
                }
                poll();
            } catch (Exception error) { report(error); }
            finally { main.post(completed); }
        });
    }
    private void owner(Object name) { if (!apk.applicationInfo.packageName.equals(name)) throw new SecurityException("Notification package is outside the current application"); }
    private void user(Object value) { if (!(value instanceof Integer) || (Integer) value != user) throw new SecurityException("Notification user is outside the current application"); }
    private static NotificationChannel copy(NotificationChannel value) {
        Parcel parcel = Parcel.obtain();
        try { value.writeToParcel(parcel, 0); parcel.setDataPosition(0); return NotificationChannel.CREATOR.createFromParcel(parcel); }
        finally { parcel.recycle(); }
    }
    synchronized Object call(Object instance, Method method, Object[] args) throws Exception {
        owner(args[0]);
        switch (method.getName()) {
            case "enqueueTextToast": return toasts.enqueue(instance,args,true);
            case "enqueueToast": return toasts.enqueue(instance,args,false);
            case "cancelToast": toasts.cancel((android.os.IBinder) args[1]); return null;
            case "finishToken": return null; // Removal is complete before ToastPresenter acknowledges its token.
            case "areNotificationsEnabled": return enabled;
            case "areNotificationsEnabledForPackage":
                if ((Integer) args[1] != apk.applicationInfo.uid) throw new SecurityException("Notification UID mismatch");
                return enabled;
            case "getPackageImportance": return enabled ? NotificationManager.IMPORTANCE_DEFAULT : NotificationManager.IMPORTANCE_NONE;
            case "createNotificationChannels": {
                List<?> values = (List<?>) args[1].getClass().getMethod("getList").invoke(args[1]);
                for (Object value : values) {
                    NotificationChannel next = copy((NotificationChannel) value);
                    if (next.getId() == null || next.getId().isEmpty() || next.getId().length() > 1024) throw new IllegalArgumentException("Invalid notification channel ID");
                    NotificationChannel previous = channels.get(next.getId());
                    if (previous != null) {
                        NotificationChannel merged = copy(previous); merged.setName(next.getName()); merged.setDescription(next.getDescription());
                        merged.setImportance(Math.min(previous.getImportance(), next.getImportance())); next = merged;
                    }
                    if (!channels.containsKey(next.getId()) && channels.size() >= 512) throw new IllegalStateException("Too many notification channels");
                    next.setBypassDnd(false); channels.put(next.getId(), next); deleted.remove(next.getId());
                }
                worker.execute(this::saveChannels); return null;
            }
            case "getNotificationChannels":
                owner(args[1]); user(args[2]);
                List<NotificationChannel> values = new ArrayList<>();
                for (NotificationChannel channel : channels.values()) if (!deleted.contains(channel.getId())) values.add(copy(channel));
                return method.getReturnType().getConstructor(List.class).newInstance(values);
            case "getNotificationChannel":
            case "getConversationNotificationChannel": {
                user(args[1]); owner(args[2]);
                NotificationChannel channel = channels.get((String) args[3]);
                if (method.getName().equals("getConversationNotificationChannel") && args[5] != null) {
                    for (NotificationChannel candidate : channels.values()) if (!deleted.contains(candidate.getId())
                            && args[3].equals(candidate.getParentChannelId()) && args[5].equals(candidate.getConversationId())) return copy(candidate);
                    if (!(Boolean) args[4]) return null;
                }
                return channel == null || deleted.contains(channel.getId()) ? null : copy(channel);
            }
            case "deleteNotificationChannel": {
                String channel = (String) args[1];
                if (channels.containsKey(channel)) deleted.add(channel);
                for (Posted posted : new ArrayList<>(active.values())) if (channel.equals(posted.notification.getChannelId())) cancel(posted.key);
                worker.execute(this::saveChannels); return null;
            }
            case "enqueueNotificationWithTag": {
                owner(args[1]); user(args[5]);
                Notification notification = (Notification) args[4];
                NotificationChannel channel = channel(notification);
                Log.d("StoneNotifications", "Enqueue id=" + args[3] + " channel=" + notification.getChannelId() + " enabled=" + enabled);
                enqueue(new Posted((String) args[2], (Integer) args[3], notification), channel); return null;
            }
            case "cancelNotificationWithTag":
                owner(args[1]); user(args[4]);
                if (args[2] != null || !foregroundId((Integer) args[3])) cancel(((String) args[2] == null ? "-" : "+" + args[2]) + ":" + args[3]); return null;
            case "cancelAllNotifications":
                user(args[1]);
                boolean protectedNotice = false;
                for (Posted posted : active.values()) if (foreground(posted)) { protectedNotice = true; break; }
                if (!protectedNotice) {
                    active.clear(); worker.execute(() -> { try { removeAllNative(); } catch (Exception error) { report(error); } });
                } else for (Posted posted : new ArrayList<>(active.values())) if (!foreground(posted)) cancel(posted.key);
                return null;
            case "getAppActiveNotifications": {
                user(args[1]); List<StatusBarNotification> result = new ArrayList<>();
                for (Posted posted : active.values()) result.add(new StatusBarNotification(apk.applicationInfo.packageName,
                    apk.applicationInfo.packageName, posted.id, posted.tag, apk.applicationInfo.uid, android.os.Process.myPid(), 0,
                    posted.notification.clone(), android.os.Process.myUserHandle(), posted.time));
                return method.getReturnType().getConstructor(List.class).newInstance(result);
            }
            case "isNotificationPolicyAccessGranted": return false;
            case "isPackagePaused": return false;
            case "canNotifyAsPackage": return apk.applicationInfo.packageName.equals(args[1]);
            default: throw new UnsupportedOperationException("ART notification service: " + method);
        }
    }
    private synchronized void cancel(String key) {
        active.remove(key);
        worker.execute(() -> { try { removeNative(key); } catch (Exception error) { report(error); } });
    }
    private void publish(Posted posted) {
        publish(posted, false);
    }
    private boolean publish(Posted posted, boolean restoring) {
        try {
            synchronized (this) { if (!enabled || active.get(posted.key) != posted) return false; }
            Notification note = posted.notification;
            String title = text(note.extras.getCharSequence(Notification.EXTRA_TITLE), 1024);
            String body = text(note.extras.getCharSequence(Notification.EXTRA_BIG_TEXT, note.extras.getCharSequence(Notification.EXTRA_TEXT)), 8192);
            if (body.isEmpty()) {
                CharSequence[] lines = note.extras.getCharSequenceArray(Notification.EXTRA_TEXT_LINES);
                if (lines != null) { StringBuilder joined = new StringBuilder();
                    for (int index = 0; index < Math.min(lines.length, 12); ++index) { if (index > 0) joined.append('\n'); joined.append(lines[index]); }
                    body = text(joined, 8192);
                }
            }
            title = text(title.isEmpty() || applicationName.equals(title) ? applicationName : applicationName + " · " + title, 1024);
            List<String> labels = new ArrayList<>();
            posted.actions.clear();
            if (note.actions != null) for (Notification.Action action : note.actions) {
                if (labels.size() >= 5) break;
                if (action.actionIntent == null || action.getRemoteInputs() != null) continue;
                labels.add(text(action.title, 256)); posted.actions.add(action.actionIntent);
            }
            NotificationChannel channel;
            synchronized (this) { channel = channels.get(note.getChannelId()); }
            boolean explicitlySilent = false;
            try { explicitlySilent = (Boolean) Notification.class.getMethod("isSilent").invoke(note); }
            catch (ReflectiveOperationException ignored) { }
            boolean lowImportance = channel == null || channel.getImportance() < NotificationManager.IMPORTANCE_DEFAULT;
            boolean updatingQuietly = posted.replacing && (note.flags & Notification.FLAG_ONLY_ALERT_ONCE) != 0;
            boolean silent = restoring || lowImportance || channel.getSound() == null || explicitlySilent || updatingQuietly;
            String[] targets = null;
            if ((note.flags & Notification.FLAG_FOREGROUND_SERVICE) == 0) try {
                List<String> ids = new ArrayList<>(); ids.add(ArtServices.pendingIntents.notificationTarget(note.contentIntent)); ids.add(ArtServices.pendingIntents.notificationTarget(note.deleteIntent));
                for (PendingIntent action : posted.actions) ids.add(ArtServices.pendingIntents.notificationTarget(action)); targets = ids.toArray(new String[0]);
            } catch (UnsupportedOperationException error) { Log.d("StoneNotifications","Notification requires its live ART: " + error.getMessage()); }
            String[] receipt = postNative(posted.key, title, body, labels.toArray(new String[0]),
                note.extras.getInt(Notification.EXTRA_PROGRESS), note.extras.getInt(Notification.EXTRA_PROGRESS_MAX),
                note.extras.getBoolean(Notification.EXTRA_PROGRESS_INDETERMINATE), silent, restoring || lowImportance || explicitlySilent || updatingQuietly,targets,note.getChannelId(),note.flags);
            if (receipt.length != 2) throw new IOException("Invalid notification receipt");
            synchronized (this) { posted.ticket = Long.parseLong(receipt[0]); posted.route = receipt[1]; if (consumed.remove(posted.route)) active.remove(posted.key,posted); }
            return true;
        } catch (Exception error) {
            boolean removed; synchronized (this) { removed = !restoring && active.remove(posted.key, posted); }
            if (removed) try { removeNative(posted.key); } catch (Exception ignored) { }
            report(error);
            return false;
        }
    }
    private static String text(CharSequence value, int limit) { String text = value == null ? "" : value.toString(); return text.substring(0, Math.min(limit, text.length())); }
    private void poll() {
        try {
            Properties policy = new Properties();
            File file = new File("/data/system/stone-notifications/policy.properties");
            if (file.isFile()) {
                if (file.length() > 1024 * 1024) throw new IOException("Notification policy is too large");
                try (FileInputStream input = new FileInputStream(file)) { policy.load(input); }
                if (!"1".equals(policy.getProperty("format"))) throw new IOException("Unsupported notification policy");
            }
            boolean permission = requestable() && "true".equals(policy.getProperty("package." + apk.applicationInfo.packageName, "false"));
            consent = permission;
            if (permission && !initialized && android.os.SystemClock.uptimeMillis() >= retryAfter) {
                retryAfter = android.os.SystemClock.uptimeMillis() + 5000;
                initialized = initializeNative(apk.applicationInfo.packageName);
            }
            boolean allowed = permission && "true".equals(policy.getProperty("global", "true")) && initialized && allowedNative();
            boolean remove;
            synchronized (this) {
                if (enabled != allowed) Log.d("StoneNotifications", "Notification delivery enabled=" + allowed + " granted=" + permission + " global=" + policy.getProperty("global", "true"));
                remove = !allowed && !active.isEmpty();
                if (remove) active.clear();
                enabled = allowed;
            }
            if (remove) removeAllNative();
            if (initialized) {
                long[] events = pollNative();
                for (int index = 0; index + 1 < events.length; index += 2) {
                    if (events[index] == 0) {
                        long next = events[index + 1];
                        if (hostGeneration != 0 && hostGeneration != next && allowed) {
                            List<Posted> restore;
                            synchronized (this) { restore = new ArrayList<>(active.values()); }
                            // A cancellation may have happened during the outage.
                            // Clear stale Windows links before replaying current records.
                            removeAllNative();
                            boolean restored = true;
                            for (Posted posted : restore) if (!publish(posted, true)) restored = false;
                            if (!restored) throw new IOException("Notification recovery is incomplete; retaining records for retry");
                            Log.i("StoneNotifications", "Restored " + restore.size() + " notifications in replacement Service");
                        }
                        hostGeneration = next;
                    } else deliver(events[index], (int) events[index + 1]);
                }
            }
            warned = false;
        } catch (Exception error) {
            // An unavailable backend does not revoke the user's policy or erase
            // the Android records needed to recover a replacement Service.
            synchronized (this) { enabled = false; }
            report(error);
        }
    }
    private void deliver(long ticket, int action) {
        Posted posted = null;
        synchronized (this) { for (Posted candidate : active.values()) if (candidate.ticket == ticket) { posted = candidate; break; } }
        if (posted == null || !enabled) return;
        Notification note = posted.notification;
        PendingIntent pending = action == -2 ? note.deleteIntent : action == -1 ? note.contentIntent
            : action >= 0 && action < posted.actions.size() ? posted.actions.get(action) : null;
        if (action == -2 && (note.flags & (Notification.FLAG_ONGOING_EVENT | Notification.FLAG_FOREGROUND_SERVICE)) != 0) return;
        if (action == -2 || action == -1 && (note.flags & Notification.FLAG_AUTO_CANCEL) != 0
                && (note.flags & Notification.FLAG_FOREGROUND_SERVICE) == 0) cancel(posted.key);
        if (pending != null) main.post(() -> { try { pending.send(); } catch (PendingIntent.CanceledException error) { Log.w("StoneNotifications", "Notification PendingIntent was canceled", error); } });
    }
    private final java.util.Set<String> consumed = new java.util.HashSet<>();
    synchronized void consumed(String key, String route, boolean cancel) {
        if (!cancel) return; Posted posted = active.get(key);
        if (posted != null && route.equals(posted.route)) active.remove(key,posted);
        else if (posted != null && posted.route.isEmpty() && consumed.size() < 256) consumed.add(route);
    }
    private void report(Exception error) { if (!warned) Log.w("StoneNotifications", "Notification operation failed", error); warned = true; }
    private void publishIdentity() throws Exception {
        String name = apk.applicationInfo.nonLocalizedLabel == null ? null : apk.applicationInfo.nonLocalizedLabel.toString();
        if (name == null && apk.applicationInfo.labelRes != 0) name = apk.resources.getString(apk.applicationInfo.labelRes);
        if (name == null || name.isEmpty()) name = apk.applicationInfo.packageName;
        applicationName = text(name, 4096);
        org.json.JSONObject entry = new org.json.JSONObject().put("packageName", apk.applicationInfo.packageName)
            .put("name", text(name, 4096)).put("uid", apk.applicationInfo.uid)
            .put("requestedPermissions",new org.json.JSONArray(apk.packageInfo.requestedPermissions==null ? java.util.Collections.emptyList() : java.util.Arrays.asList(apk.packageInfo.requestedPermissions)))
            .put("storageIdentity",ArtStorage.identity(apk.packageInfo));
        byte[] bytes = new org.json.JSONObject().put("format", 1).put("apps", new org.json.JSONArray().put(entry))
            .toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
        AtomicFile file = new AtomicFile(new File("/data/system/stone-notifications/apps/" + apk.applicationInfo.packageName + ".json"));
        FileOutputStream output = file.startWrite();
        try { output.write(bytes); file.finishWrite(output); }
        catch (Exception error) { file.failWrite(output); throw error; }
    }
    static void forget(String name) throws IOException {
        if (!name.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+")) throw new IOException("Invalid notification package");
        File observed = new File("/data/system/stone-notifications/apps/" + name + ".json");
        if (observed.exists() && !observed.delete()) throw new IOException("Cannot remove notification application identity");
        File policy = new File("/data/system/stone-notifications/policy.properties");
        if (!policy.exists()) return;
        try (java.io.RandomAccessFile gate = new java.io.RandomAccessFile(new File(policy.getParentFile(), "policy.lock"), "rw");
                java.nio.channels.FileLock lock = gate.getChannel().lock(0, 1, false)) {
            Properties state = new Properties();
            try (FileInputStream input = new FileInputStream(policy)) { state.load(input); }
            if (!"1".equals(state.getProperty("format"))) throw new IOException("Invalid notification policy");
            if (state.remove("package." + name) == null) return;
            AtomicFile file = new AtomicFile(policy); FileOutputStream output = file.startWrite();
            try { state.store(output, "Stone notification permissions"); file.finishWrite(output); }
            catch (Exception error) { file.failWrite(output); throw error; }
        }
    }
    private File channelFile() { return new File(apk.applicationInfo.dataDir, ".stone-notification-channels.properties"); }
    private synchronized void loadChannels() throws IOException {
        File file = channelFile(); if (!file.isFile()) return;
        if (file.length() > 2 * 1024 * 1024) throw new IOException("Notification channels are too large");
        Properties state = new Properties(); try (FileInputStream input = new FileInputStream(file)) { state.load(input); }
        if (!"1".equals(state.getProperty("format"))) throw new IOException("Invalid notification channels");
        for (String key : state.stringPropertyNames()) if (key.startsWith("channel.")) {
            byte[] bytes = Base64.decode(state.getProperty(key), Base64.DEFAULT);
            Parcel parcel = Parcel.obtain();
            try { parcel.unmarshall(bytes, 0, bytes.length); parcel.setDataPosition(0); NotificationChannel channel = NotificationChannel.CREATOR.createFromParcel(parcel);
                channels.put(channel.getId(), channel); if ("true".equals(state.getProperty("deleted." + key.substring(8)))) deleted.add(channel.getId()); }
            finally { parcel.recycle(); }
        }
    }
    private void saveChannels() {
        try {
            Properties state = new Properties(); state.setProperty("format", "1");
            synchronized (this) { for (NotificationChannel channel : channels.values()) {
                String key = Base64.encodeToString(channel.getId().getBytes(java.nio.charset.StandardCharsets.UTF_8), Base64.NO_WRAP);
                Parcel parcel = Parcel.obtain();
                try { channel.writeToParcel(parcel, 0); state.setProperty("channel." + key, Base64.encodeToString(parcel.marshall(), Base64.NO_WRAP)); }
                finally { parcel.recycle(); }
                if (deleted.contains(channel.getId())) state.setProperty("deleted." + key, "true");
            } }
            AtomicFile file = new AtomicFile(channelFile()); FileOutputStream output = file.startWrite();
            try { state.store(output, "Stone notification channels"); file.finishWrite(output); }
            catch (Exception error) { file.failWrite(output); throw error; }
        } catch (Exception error) { report(error); }
    }
}
