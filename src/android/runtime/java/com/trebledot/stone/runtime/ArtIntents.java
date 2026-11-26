package com.trebledot.stone.runtime;

import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.util.Base64;
import android.util.Log;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

/** PendingIntent identity and delivery for local activities and broadcasts. */
final class ArtIntents {
    private static native boolean available();
    private static native String[] exchange(String owner, String[] fields) throws java.io.IOException;
    private final ArtPackage apk;
    private final int user;
    private final ArtBroadcasts broadcasts;
    private final boolean remote = available();
    private final ClassLoader loader;
    private final java.util.concurrent.atomic.AtomicInteger deliveries = new java.util.concurrent.atomic.AtomicInteger();
    private final android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());
    private boolean attached;
    private final Object deliveryGate = new Object();
    private boolean retired;
    private java.util.concurrent.ScheduledExecutorService deliveryWorker;
    private volatile boolean warned;
    private final Map<Key, Record> records = new HashMap<>();
    private final Map<IBinder, Record> tokens = new HashMap<>();
    private static final int BROADCAST = 1, ACTIVITY = 2;

    private static final class Key {
        final int type, code, flags;
        final Intent.FilterComparison intent;
        final String resolvedType;
        Key(int type, int code, int flags, Intent intent, String resolvedType) {
            this.type = type; this.code = code; this.flags = flags;
            this.intent = new Intent.FilterComparison(intent.cloneFilter()); this.resolvedType = resolvedType;
        }
        @Override public int hashCode() { return Objects.hash(type, code, flags, intent, resolvedType); }
        @Override public boolean equals(Object other) {
            if (!(other instanceof Key)) return false;
            Key key = (Key) other;
            return type == key.type && code == key.code && flags == key.flags && intent.equals(key.intent) && Objects.equals(resolvedType, key.resolvedType);
        }
    }
    private static final class Record {
        final Key key;
        Intent intent;
        IInterface sender;
        final String id;
        boolean cancelled, persistent;
        Record(Key key, Intent intent, String id) { this.key = key; this.intent = new Intent(intent); this.id = id; }
    }

    ArtIntents(ArtPackage apk, int user, ArtBroadcasts broadcasts, ClassLoader loader) throws ReflectiveOperationException {
        this.apk = apk; this.user = user; this.broadcasts = broadcasts; this.loader = loader;
        if (remote) {
            String after = "";
            for (int count = 0; ; ++count) {
                String[] row = request("pi-list", after);
                if (row.length == 0) break;
                if (count >= 8192 || row.length != 3 || row[0].compareTo(after) <= 0)
                    throw new IllegalStateException("Invalid saved PendingIntent page");
                after = row[0]; Record record = decode(after, row[2], loader);
                if (Integer.toUnsignedLong(record.key.flags) != Long.parseLong(row[1])) throw new IllegalStateException("Saved PendingIntent flags changed");
                record.persistent = true; bind(record);
                if (records.put(record.key, record) != null) throw new IllegalStateException("Duplicate saved PendingIntent identity");
            }
        }
    }
    synchronized String notificationTarget(PendingIntent intent) throws ReflectiveOperationException {
        if (intent == null) return "";
        if (!remote) throw new UnsupportedOperationException("Standalone PendingIntent requires its live ART");
        IInterface target = (IInterface) PendingIntent.class.getMethod("getTarget").invoke(intent);
        Record record = tokens.get(target.asBinder());
        if (record == null || !record.persistent) throw new UnsupportedOperationException("Notification PendingIntent cannot be restored");
        return record.id;
    }
    synchronized void attach() {
        if (!remote || attached) return; attached = true;
        deliveryWorker = java.util.concurrent.Executors.newSingleThreadScheduledExecutor(action -> {
            Thread thread = new Thread(action,"Stone notification intents"); thread.setDaemon(true); return thread;
        });
        deliveryWorker.scheduleWithFixedDelay(this::pollDelivery,0,250,java.util.concurrent.TimeUnit.MILLISECONDS);
    }
    boolean hasWork() { return deliveries.get() != 0; }
    boolean retireIfIdle() {
        synchronized (deliveryGate) {
            if (hasWork()) return false;
            boolean granted = ArtServices.jobs.retireIfIdle(); if (granted) retired = true; return granted;
        }
    }
    private void pollDelivery() {
        synchronized (deliveryGate) {
        if (retired) return;
        try {
            String[] row = request("pi-delivery",""); if (row.length == 0) return;
            if (row.length != 5) throw new IllegalStateException("Invalid notification delivery");
            Record record = decode("",row[1],loader);
            deliveries.incrementAndGet(); boolean posted = false;
            try {
                // Claim durably before dispatch: never replay a claimed event.
                // A crash between this claim and ART execution can lose it.
                if (!accepted("pi-claim",row[0])) return;
                posted = main.post(() -> {
                    try {
                        ArtServices.notifications.consumed(row[2],row[3],row[4].equals("1"));
                        int result = send(record,0,null,null,null,null);
                        Log.i("StoneIntents","Notification delivery " + row[0] + " dispatched: " + result);
                    } catch (ReflectiveOperationException | RuntimeException error) { Log.e("StoneIntents","Notification Intent dispatch failed",error); }
                    finally {
                        // Activity/Receiver dispatch itself queues work on the
                        // main Looper. Keep retirement blocked through it.
                        if (!main.post(() -> deliveries.decrementAndGet())) deliveries.decrementAndGet();
                    }
                });
                if (!posted) throw new IllegalStateException("Notification main Looper is shutting down");
            } finally { if (!posted) deliveries.decrementAndGet(); }
            warned = false;
        } catch (RuntimeException error) { if (!warned) Log.w("StoneIntents","Notification delivery unavailable",error); warned = true; }
        }
    }
    private String[] request(String... fields) {
        try { return exchange(apk.applicationInfo.packageName, fields); }
        catch (java.io.IOException error) { throw new IllegalStateException("PendingIntent service operation failed", error); }
    }
    private boolean accepted(String... fields) {
        String[] reply = request(fields);
        if (reply.length != 1 || !reply[0].equals("1") && !reply[0].equals("0")) throw new IllegalStateException("Invalid PendingIntent service response");
        return reply[0].equals("1");
    }
    private void validate(Intent intent) {
        String owner = apk.applicationInfo.packageName; ComponentName component = intent.getComponent();
        if (component != null && !owner.equals(component.getPackageName()) || component == null && !owner.equals(intent.getPackage()))
            throw new SecurityException("PendingIntent must target the local application");
    }
    private static String encode(Record record, Intent intent) {
        Parcel parcel = Parcel.obtain();
        try {
            ArtSnapshotParcel.install(parcel);
            parcel.writeInt(1); parcel.writeInt(record.key.type); parcel.writeInt(record.key.code); parcel.writeInt(record.key.flags);
            parcel.writeString(record.key.resolvedType); intent.writeToParcel(parcel, 0);
            if (parcel.hasFileDescriptors()) throw new IllegalArgumentException("PendingIntent contains file descriptors");
            String payload = Base64.encodeToString(parcel.marshall(), Base64.NO_WRAP);
            if (payload.length() > 24576) throw new IllegalArgumentException("PendingIntent payload is too large");
            return payload;
        } finally { parcel.recycle(); }
    }
    private Record decode(String id, String payload, ClassLoader loader) {
        byte[] bytes = Base64.decode(payload, Base64.NO_WRAP); Parcel parcel = Parcel.obtain();
        try {
            parcel.unmarshall(bytes, 0, bytes.length); parcel.setDataPosition(0);
            if (parcel.readInt() != 1) throw new IllegalStateException("Unknown saved PendingIntent version");
            int type = parcel.readInt(), code = parcel.readInt(), flags = parcel.readInt(); String resolvedType = parcel.readString();
            Intent intent = Intent.CREATOR.createFromParcel(parcel); intent.setExtrasClassLoader(loader); validate(intent);
            if (type != ACTIVITY && type != BROADCAST || parcel.dataAvail() != 0
                    || (flags & (PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_UPDATE_CURRENT)) != 0)
                throw new IllegalStateException("Invalid saved PendingIntent");
            return new Record(new Key(type, code, flags, intent, resolvedType), intent, id);
        } finally { parcel.recycle(); }
    }
    private static String snapshot(Record record, Intent intent) {
        try { return encode(record, intent); }
        catch (RuntimeException error) {
            Log.w("StoneIntents", "PendingIntent requires the live ART process; its Parcel cannot be persisted", error); return null;
        }
    }
    private void persist(Record record) {
        if (!remote) return;
        String payload = snapshot(record, record.intent); if (payload == null) return;
        // Retain this identity locally even if IPC loses its response. A later
        // create/check can reconcile the same ID instead of publishing a second
        // durable record for the same Android identity.
        record.persistent = true;
        if (!accepted("pi-create", record.id, Integer.toUnsignedString(record.key.flags), payload)) throw new IllegalStateException("PendingIntent identity was already canceled");
    }
    private void bind(Record record) throws ReflectiveOperationException {
        String descriptor = "android.content.IIntentSender";
        record.sender = ArtServices.service(descriptor, (instance, method, arguments) -> {
            if (!method.getName().equals("send")) throw new UnsupportedOperationException("ART IntentSender: " + method.getName());
            send(record, (Integer) arguments[0], (Intent) arguments[1], (String) arguments[2], (IInterface) arguments[4], (String) arguments[5]);
            return null;
        }).queryLocalInterface(descriptor);
        tokens.put(record.sender.asBinder(), record);
    }

    synchronized IInterface create(Object[] args) throws ReflectiveOperationException {
        int type = (Integer) args[0], flags = (Integer) args[8];
        boolean lookupOnly = (flags & PendingIntent.FLAG_NO_CREATE) != 0;
        Intent[] intents = (Intent[]) args[6];
        if (type != ACTIVITY && type != BROADCAST || intents == null || intents.length != 1) {
            if (lookupOnly) return null;
            throw new UnsupportedOperationException("ART PendingIntent requires one local activity or broadcast");
        }
        String owner = apk.applicationInfo.packageName;
        Intent intent = intents[0];
        ComponentName component = intent.getComponent();
        if (!owner.equals(args[1]) || (Integer) args[10] != user
                || component != null && !owner.equals(component.getPackageName())
                || component == null && !owner.equals(intent.getPackage()))
            throw new SecurityException("PendingIntent must target the local application and user");
        int identityFlags = flags & ~(PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_UPDATE_CURRENT);
        String[] types = (String[]) args[7];
        Key key = new Key(type, (Integer) args[5], identityFlags, intent, types == null ? null : types[0]);
        Record existing = records.get(key);
        if (existing != null && existing.persistent && !accepted("pi-check", existing.id)) { cancelLocal(existing); existing = null; }
        if ((flags & PendingIntent.FLAG_CANCEL_CURRENT) != 0 && existing != null) { cancel(existing); existing = null; }
        if (existing != null) {
            if ((flags & PendingIntent.FLAG_UPDATE_CURRENT) != 0) {
                Intent updated = new Intent(existing.intent); updated.replaceExtras(intent);
                String payload = existing.persistent ? snapshot(existing, updated) : null;
                if (existing.persistent && payload == null) {
                    // Keep a valid live PendingIntent when new extras acquire a
                    // Binder or FD, but revoke its now-inaccurate durable copy.
                    request("pi-cancel", existing.id); existing.persistent = false;
                }
                if (existing.persistent && !accepted("pi-update", existing.id, Integer.toUnsignedString(existing.key.flags), payload)) {
                    cancelLocal(existing); existing = null;
                } else existing.intent = updated;
            }
            if (existing != null) return existing.sender;
        }
        if (lookupOnly) return null;
        Record record = new Record(key, intent, java.util.UUID.randomUUID().toString().replace("-", ""));
        bind(record);
        records.put(key, record);
        persist(record);
        return record.sender;
    }

    synchronized void cancel(IInterface sender) { Record record = tokens.get(sender.asBinder()); if (record != null) cancel(record); }
    private void cancelLocal(Record record) { record.cancelled = true; records.remove(record.key, record); }
    private void cancel(Record record) { if (record.persistent) request("pi-cancel", record.id); cancelLocal(record); }

    synchronized Object info(IInterface sender) throws ReflectiveOperationException {
        Record record = tokens.get(sender.asBinder());
        return Class.forName("android.app.ActivityManager$PendingIntentInfo")
            .getConstructor(String.class, int.class, boolean.class, int.class)
            .newInstance(record == null ? null : apk.applicationInfo.packageName, record == null ? -1 : apk.applicationInfo.uid,
                record != null && (record.key.flags & PendingIntent.FLAG_IMMUTABLE) != 0, record == null ? 0 : record.key.type);
    }

    int send(Object[] args) throws ReflectiveOperationException {
        IInterface target = (IInterface) args[1];
        Record record;
        synchronized (this) { record = tokens.get(target.asBinder()); }
        if (record != null) return send(record, (Integer) args[3], (Intent) args[4], (String) args[5], (IInterface) args[6], (String) args[7]);
        Class.forName("android.content.IIntentSender").getMethod("send", int.class, Intent.class, String.class, IBinder.class,
            Class.forName("android.content.IIntentReceiver"), String.class, Bundle.class)
            .invoke(target, args[3], args[4], args[5], args[2], args[6], args[7], args[8]);
        return 0;
    }

    private int send(Record record, int code, Intent extra, String resolvedType, IInterface finished, String requiredPermission)
            throws ReflectiveOperationException {
        Intent intent;
        synchronized (this) {
            if (record.cancelled) return -96;
            if (requiredPermission != null) throw new UnsupportedOperationException("Permission-filtered PendingIntent delivery is not connected yet");
            if (record.persistent) {
                try { if (!accepted("pi-consume", record.id)) { cancelLocal(record); return -96; } }
                catch (IllegalStateException error) {
                    // Do not dispatch without the service's decision, or crash
                    // the notification main callback on a transient IPC error.
                    // Keep the local record so a later check can reconcile it.
                    Log.w("StoneIntents", "PendingIntent delivery could not be confirmed by Service", error); return -96;
                }
            }
            intent = new Intent(record.intent);
            if (extra != null && (record.key.flags & PendingIntent.FLAG_IMMUTABLE) == 0) intent.fillIn(extra, record.key.flags);
            if ((record.key.flags & PendingIntent.FLAG_ONE_SHOT) != 0) cancelLocal(record);
        }
        String type = record.key.resolvedType == null ? resolvedType : record.key.resolvedType;
        int result = record.key.type == ACTIVITY
            ? ArtServices.activities.start(apk.applicationInfo.packageName, intent, null, null, -1) : 0;
        if (record.key.type == BROADCAST) broadcasts.dispatch(intent, type, apk.applicationInfo.uid, apk.applicationInfo.packageName);
        if (result >= 0 && finished != null) Class.forName("android.content.IIntentReceiver")
            .getMethod("performReceive", Intent.class, int.class, String.class, Bundle.class, boolean.class, boolean.class, int.class)
            .invoke(finished, intent, code, null, null, false, false, user);
        return result;
    }
}
