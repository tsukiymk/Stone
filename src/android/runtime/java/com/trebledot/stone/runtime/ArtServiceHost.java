package com.trebledot.stone.runtime;

import android.app.ActivityManager;
import android.app.Notification;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ComponentInfo;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.SystemClock;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Local service ownership; the image ActivityThread creates and calls the actual APK Service. */
final class ArtServiceHost {
    private final ArtPackage apk;
    private final int user;
    private ArtApplication application;
    private final Map<ComponentName, Record> services = new LinkedHashMap<>();
    private final Map<IBinder, Record> tokens = new HashMap<>();
    private final Map<IBinder, List<Connection>> connections = new HashMap<>();

    private static final class Record {
        final ServiceInfo info;
        final ComponentName name;
        final IBinder token = new Binder();
        final long created = SystemClock.elapsedRealtime();
        final Map<Intent.FilterComparison, Binding> bindings = new HashMap<>();
        final ArrayDeque<Binding> unbinding = new ArrayDeque<>(), rebinding = new ArrayDeque<>();
        int startId, foregroundType;
        boolean started, stopping, foreground;
        Record(ServiceInfo info) { this.info = info; name = new ComponentName(info.packageName, info.name); }
    }
    private static final class Binding {
        final Record service;
        final IBinder token = new Binder();
        final Intent intent;
        final List<Connection> clients = new ArrayList<>();
        IBinder binder;
        boolean published, pending, unbinding, rebind;
        Binding(Record service, Intent intent) { this.service = service; this.intent = new Intent(intent); }
    }
    private static final class Connection {
        final Binding binding;
        final IInterface receiver;
        final long flags;
        Connection(Binding binding, IInterface receiver, long flags) { this.binding = binding; this.receiver = receiver; this.flags = flags; }
    }

    ArtServiceHost(ArtPackage apk, int user) { this.apk = apk; this.user = user; }
    synchronized void attach(ArtApplication application) { this.application = application; }
    synchronized boolean hasWork() { return !services.isEmpty() || !tokens.isEmpty(); }

    synchronized int importance() {
        for (Record record : services.values())
            if (record.foreground) return ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND_SERVICE;
        return services.isEmpty() ? ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED : ActivityManager.RunningAppProcessInfo.IMPORTANCE_SERVICE;
    }

    synchronized Object call(Object proxy, Method method, Object[] args) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "startService": {
                owner((String) args[4], (Integer) args[6]);
                ServiceInfo info = resolve((Intent) args[1]);
                if (info == null) return null;
                Record record = obtain(info);
                record.started = true;
                if (record.startId == Integer.MAX_VALUE) throw new IllegalStateException("Service start IDs exhausted");
                Class<?> startType = Class.forName("android.app.ServiceStartArgs");
                Object start = startType.getConstructor(boolean.class, int.class, int.class, Intent.class)
                    .newInstance(false, ++record.startId, 0, new Intent((Intent) args[1]));
                Class<?> slice = Class.forName("android.content.pm.ParceledListSlice");
                schedule("scheduleServiceArgs", new Class<?>[]{IBinder.class, slice}, record.token,
                    slice.getConstructor(List.class).newInstance(java.util.Collections.singletonList(start)));
                return record.name;
            }
            case "stopService": {
                checkUser((Integer) args[3]);
                ServiceInfo info = resolve((Intent) args[1]);
                Record record = info == null ? null : services.get(new ComponentName(info.packageName, info.name));
                if (record == null || !record.started) return 0;
                record.started = false;
                stopIfUnused(record);
                return 1;
            }
            case "stopServiceToken": {
                Record record = tokens.get((IBinder) args[1]);
                if (record == null) return false;
                if (!record.name.equals(args[0])) throw new SecurityException("Service component does not match token");
                int id = (Integer) args[2];
                if (!record.started || id >= 0 && id != record.startId) return false;
                record.started = false;
                stopIfUnused(record);
                return true;
            }
            case "bindServiceInstance": return bind(args);
            case "unbindService": return unbind((IInterface) args[0]);
            case "publishService": {
                Record record = token((IBinder) args[0]);
                Binding binding = binding(record, (IBinder) args[1]);
                binding.binder = (IBinder) args[2];
                binding.published = true;
                binding.pending = false;
                if (!binding.unbinding && !record.stopping) deliver(binding);
                return null;
            }
            case "unbindFinished": {
                Record record = token((IBinder) args[0]);
                Binding binding = binding(record, (IBinder) args[1]);
                record.unbinding.remove(binding);
                unbound(binding, true);
                return null;
            }
            case "serviceDoneExecuting": {
                Record record = token((IBinder) args[0]);
                switch ((Integer) args[1]) {
                    case 2: tokens.remove(record.token); break; // STOP
                    case 3: { // REBIND
                        Binding binding = record.rebinding.removeFirst();
                        binding.pending = false;
                        if (!binding.unbinding && !record.stopping) deliver(binding);
                        break;
                    }
                    case 4: unbound(record.unbinding.removeFirst(), false); break; // UNBIND
                    default: break; // Creation/start completion; Android owns invocation and return value.
                }
                return null;
            }
            case "setServiceForeground": {
                Record record = token((IBinder) args[1]);
                if (!record.name.equals(args[0])) throw new SecurityException("Service component does not match token");
                int id = (Integer) args[2];
                if (id != 0) {
                    if (args[3] == null) throw new IllegalArgumentException("Foreground service requires a notification");
                    int type = (Integer) args[5];
                    if (type == -1) type = record.info.getForegroundServiceType();
                    if ((type & ~record.info.getForegroundServiceType()) != 0)
                        throw new IllegalArgumentException("Foreground service type is not declared in the manifest");
                    ArtServices.notifications.startForeground(record.token, id, (Notification) args[3]);
                    record.foregroundType = type;
                    record.foreground = true;
                } else {
                    ArtServices.notifications.stopForeground(record.token, (Integer) args[4]);
                    record.foregroundType = 0; record.foreground = false;
                }
                return null;
            }
            case "getForegroundServiceType": {
                Record record = token((IBinder) args[1]);
                if (!record.name.equals(args[0])) throw new SecurityException("Service component does not match token");
                return record.foregroundType;
            }
            case "getServices": {
                List<ActivityManager.RunningServiceInfo> result = new ArrayList<>();
                int max = (Integer) args[0];
                for (Record record : services.values()) {
                    if (result.size() >= max) break;
                    ActivityManager.RunningServiceInfo info = new ActivityManager.RunningServiceInfo();
                    info.service = record.name; info.pid = android.os.Process.myPid(); info.uid = apk.applicationInfo.uid;
                    info.process = ArtProcess.current(apk); info.started = record.started;
                    info.foreground = record.foreground; info.activeSince = record.created;
                    info.lastActivityTime = SystemClock.uptimeMillis();
                    for (Binding binding : record.bindings.values()) info.clientCount += binding.clients.size();
                    result.add(info);
                }
                return result;
            }
            default: throw new UnsupportedOperationException("ART local service: " + method.getName());
        }
    }

    private void owner(String name, int requestedUser) {
        checkUser(requestedUser);
        if (!apk.applicationInfo.packageName.equals(name)) throw new SecurityException("Service caller is outside the local application");
    }
    private void checkUser(int requestedUser) {
        if (requestedUser != user) throw new SecurityException("Services are restricted to the current user");
    }
    private ServiceInfo resolve(Intent intent) {
        ComponentName name = intent.getComponent();
        if (name == null) throw new UnsupportedOperationException("Implicit service resolution is not connected yet");
        if (!apk.applicationInfo.packageName.equals(name.getPackageName())) return null;
        ComponentInfo info = apk.components.find(name);
        if (!(info instanceof ServiceInfo) || !apk.components.enabled(info)) return null;
        if (!ArtProcess.current(apk).equals(info.processName) || (((ServiceInfo) info).flags & ServiceInfo.FLAG_ISOLATED_PROCESS) != 0)
            throw new UnsupportedOperationException("Separate-process and isolated services are not connected yet");
        return new ServiceInfo((ServiceInfo) info);
    }
    private Record obtain(ServiceInfo info) throws ReflectiveOperationException {
        ComponentName name = new ComponentName(info.packageName, info.name);
        Record record = services.get(name);
        if (record != null) return record;
        if (application == null) throw new IllegalStateException("Application service dispatcher is not ready");
        record = new Record(info);
        Class<?> compatibility = Class.forName("android.content.res.CompatibilityInfo");
        schedule("scheduleCreateService", new Class<?>[]{IBinder.class, ServiceInfo.class, compatibility, int.class},
            record.token, info, compatibility.getField("DEFAULT_COMPATIBILITY_INFO").get(null), 2);
        services.put(name, record);
        tokens.put(record.token, record);
        return record;
    }
    private Record token(IBinder token) {
        Record record = tokens.get(token);
        if (record == null) throw new SecurityException("Unknown service token");
        return record;
    }
    private Binding binding(Record record, IBinder token) {
        for (Binding binding : record.bindings.values()) if (binding.token == token) return binding;
        throw new SecurityException("Unknown service binding token");
    }
    private int bind(Object[] args) throws ReflectiveOperationException {
        owner((String) args[7], (Integer) args[8]);
        long flags = ((Number) args[5]).longValue();
        if (args[6] != null || (flags & 0x80000000L) != 0) throw new UnsupportedOperationException("External and named service instances are not connected yet");
        Intent intent = (Intent) args[2];
        ServiceInfo info = resolve(intent);
        if (info == null) return 0;
        Record record = services.get(new ComponentName(info.packageName, info.name));
        if (record == null && (flags & Context.BIND_AUTO_CREATE) == 0) return 0;
        if (record == null) record = obtain(info);
        Intent.FilterComparison key = new Intent.FilterComparison(intent.cloneFilter());
        Binding binding = record.bindings.get(key);
        if (binding == null) { binding = new Binding(record, intent); record.bindings.put(key, binding); }
        IInterface receiver = (IInterface) args[4];
        List<Connection> list = connections.get(receiver.asBinder());
        if (list == null) { list = new ArrayList<>(); connections.put(receiver.asBinder(), list); }
        for (Connection existing : list) if (existing.binding == binding) return 1;
        Connection connection = new Connection(binding, receiver, flags);
        binding.clients.add(connection); list.add(connection);
        if (!binding.unbinding && !binding.pending) {
            if (binding.published && !binding.rebind) connected(connection, false);
            else requestBind(binding);
        }
        return 1;
    }
    private void requestBind(Binding binding) throws ReflectiveOperationException {
        binding.pending = true;
        boolean rebind = binding.published && binding.rebind;
        binding.rebind = false;
        if (rebind) binding.service.rebinding.add(binding);
        schedule("scheduleBindService", new Class<?>[]{IBinder.class, IBinder.class, Intent.class, boolean.class, int.class, long.class},
            binding.service.token, binding.token, new Intent(binding.intent), rebind, 2, 0L);
    }
    private boolean unbind(IInterface receiver) throws ReflectiveOperationException {
        List<Connection> list = connections.remove(receiver.asBinder());
        if (list == null) return false;
        for (Connection connection : list) {
            Binding binding = connection.binding;
            binding.clients.remove(connection);
            if (binding.clients.isEmpty() && !binding.unbinding && !binding.service.stopping) {
                binding.unbinding = true;
                binding.service.unbinding.add(binding);
                schedule("scheduleUnbindService", new Class<?>[]{IBinder.class, IBinder.class, Intent.class},
                    binding.service.token, binding.token, new Intent(binding.intent));
            }
        }
        return true;
    }
    private void unbound(Binding binding, boolean rebind) throws ReflectiveOperationException {
        binding.unbinding = false;
        binding.rebind = rebind;
        if (!rebind) { binding.published = false; binding.binder = null; }
        if (!binding.service.stopping && !binding.clients.isEmpty()) requestBind(binding);
        stopIfUnused(binding.service);
    }
    private void stopIfUnused(Record record) throws ReflectiveOperationException {
        if (record.started || record.stopping || !record.unbinding.isEmpty()) return;
        for (Binding binding : record.bindings.values()) for (Connection connection : binding.clients)
            if ((connection.flags & Context.BIND_AUTO_CREATE) != 0) return;
        record.stopping = true;
        ArtServices.notifications.destroyService(record.token);
        record.foreground = false; record.foregroundType = 0;
        services.remove(record.name, record);
        for (Binding binding : record.bindings.values()) for (Connection connection : binding.clients) connected(connection, true);
        schedule("scheduleStopService", new Class<?>[]{IBinder.class}, record.token);
    }
    private void deliver(Binding binding) throws ReflectiveOperationException {
        for (Connection connection : new ArrayList<>(binding.clients)) connected(connection, false);
    }
    private void connected(Connection connection, boolean dead) throws ReflectiveOperationException {
        Class.forName("android.app.IServiceConnection").getMethod("connected", ComponentName.class, IBinder.class,
            Class.forName("android.app.IBinderSession"), boolean.class).invoke(connection.receiver,
                connection.binding.service.name, dead ? null : connection.binding.binder, null, dead);
    }
    private void schedule(String name, Class<?>[] signature, Object... args) throws ReflectiveOperationException {
        application.scheduleService(name, signature, args);
    }
}
