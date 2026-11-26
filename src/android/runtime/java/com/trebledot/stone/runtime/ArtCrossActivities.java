package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** ART owns Android objects; Service owns authenticated cross-process routes. */
final class ArtCrossActivities {
    private static native boolean available();
    private static native String[] exchange(String owner, String[] fields) throws IOException;
    private final ArtApplication application;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Map<String,Outgoing> outgoing = new HashMap<>();
    private final Map<IBinder,Incoming> incoming = new HashMap<>();
    private final AtomicInteger work = new AtomicInteger();
    private final ScheduledExecutorService worker;
    private String owner;
    private boolean warned;
    private static final class Outgoing {
        final IBinder token; final String who; final int request; final boolean local;
        Outgoing(IBinder token, String who, int request,boolean local) { this.token = token; this.who = who; this.request = request; this.local=local; }
    }
    private static final class Incoming {
        final String route; final boolean local; ComponentName caller;
        Incoming(String route, ComponentName caller, boolean local) { this.route = route; this.caller = caller; this.local = local; }
    }
    ArtCrossActivities(ArtApplication application) {
        this.application = application;
        worker = available() ? Executors.newSingleThreadScheduledExecutor(action -> {
            Thread thread = new Thread(action,"Stone Activity routes"); thread.setDaemon(true); return thread;
        }) : null;
        if (worker != null) worker.scheduleWithFixedDelay(this::poll,0,250,TimeUnit.MILLISECONDS);
    }
    boolean enabled() { return worker != null; }
    void close() { if (worker != null) worker.shutdownNow(); }
    boolean hasWork() { return work.get() != 0; }
    private void execute(Runnable action) {
        work.incrementAndGet(); worker.execute(() -> { try { action.run(); } finally { work.decrementAndGet(); } });
    }
    private String[] request(String... fields) throws IOException {
        String[] reply = exchange(application.context.getPackageName(),fields);
        if (reply.length == 0) throw new IOException("Activity Service returned no identity");
        if (owner != null && !owner.equals(reply[0])) main.post(() -> {
            // Routes are transient. A replacement Service cannot restore their
            // Android callers; cancel locally and revoke any uncertain new start.
            for (String key : new java.util.ArrayList<>(outgoing.keySet())) {
                Outgoing call = outgoing.remove(key); deliver(call,Activity.RESULT_CANCELED,null);
                execute(() -> { try { request("abandon",key); } catch (IOException error) { Log.w("StoneActivities","Could not revoke obsolete request",error); } });
            }
            for (Incoming call : incoming.values()) call.caller = null;
        });
        owner = reply[0]; return java.util.Arrays.copyOfRange(reply,1,reply.length);
    }
    static void accessible(ActivityInfo info, String owner) {
        if (info == null || !info.exported || info.permission != null || !info.enabled || !info.applicationInfo.enabled)
            throw new SecurityException("Activity is not accessible to another application");
        if (!info.packageName.equals(owner) || !info.applicationInfo.processName.equals(info.processName)
                || info.launchMode != ActivityInfo.LAUNCH_MULTIPLE)
            throw new UnsupportedOperationException("Cross-process Activity requires a standard Activity in the application's main process");
    }
    private void accessibleLocal(ActivityInfo info) {
        if (info == null || !info.packageName.equals(application.context.getPackageName()) || !info.enabled || !info.applicationInfo.enabled)
            throw new SecurityException("Private-process Activity is not enabled in this APK");
        String process = info.processName;
        if ((!process.equals(info.applicationInfo.processName) && !process.matches(java.util.regex.Pattern.quote(info.packageName)+":[A-Za-z0-9_]+"))
                || info.launchMode != ActivityInfo.LAUNCH_MULTIPLE)
            throw new UnsupportedOperationException("Private-process Activity requires a declared standard Activity");
    }
    int startLocal(Intent intent, ActivityInfo info, IBinder token, String who, int request, ComponentName caller) {
        accessibleLocal(info);
        try { return queue(info,intent,ArtActivityIntent.encodeLocal(intent),true,token,who,request,caller); }
        catch (IOException error) { throw new IllegalArgumentException("Cannot encode private Activity request",error); }
    }
    int start(Intent intent, IBinder token, String who, int request, ComponentName caller) {
        if (!enabled() || intent == null || intent.getComponent() == null) return -91;
        try {
            ActivityInfo info = application.context.getPackageManager().getActivityInfo(intent.getComponent(),0);
            accessible(info,intent.getComponent().getPackageName());
            return queue(info,intent,ArtActivityIntent.encode(intent),false,token,who,request,caller);
        } catch (PackageManager.NameNotFoundException missing) { return -92; }
        catch (IOException error) { throw new IllegalArgumentException("Cannot encode Activity request",error); }
    }
    private int queue(ActivityInfo info, Intent intent, String payload, boolean local, IBinder token, String who, int request, ComponentName caller) {
            String key = java.util.UUID.randomUUID().toString().replace("-","");
            if (request >= 0) outgoing.put(key,new Outgoing(token,who,request,local));
            execute(() -> {
                try {
                    if (local) request("start",key,caller.flattenToString(),info.packageName,payload,request >= 0 ? "1" : "0",
                        info.processName.equals(info.applicationInfo.processName) ? "" : info.processName);
                    else request("start",key,caller.flattenToString(),info.packageName,payload,request >= 0 ? "1" : "0");
                    if(local && request>=0) main.post(() -> { if(outgoing.containsKey(key)) ArtWindow.setVisible(false); });
                } catch (IOException error) {
                    Log.w("StoneActivities","Cross-process Activity launch failed",error);
                    main.post(() -> deliver(outgoing.remove(key),Activity.RESULT_CANCELED,null));
                }
            });
            return 0;
    }
    void attach(IBinder token, String route, ComponentName caller) { incoming.put(token,new Incoming(route,caller,localRoutes.remove(route))); }
    private final java.util.Set<String> localRoutes = new java.util.HashSet<>();
    ComponentName caller(IBinder token) { Incoming call = incoming.get(token); return call == null ? null : call.caller; }
    boolean isIncoming(IBinder token) { return incoming.containsKey(token); }
    Intent copyResult(IBinder token, Intent data) {
        if (!incoming.containsKey(token)) return data;
        try { return incoming.get(token).local ? ArtActivityIntent.decodeLocal(ArtActivityIntent.encodeLocal(data),application.context.getClassLoader())
            : ArtActivityIntent.decode(ArtActivityIntent.encode(data)); }
        catch (IOException error) { throw new IllegalArgumentException("Cannot snapshot Activity result",error); }
    }
    void finish(IBinder token, int result, Intent data) {
        Incoming call = incoming.get(token); if (call == null) return;
        final String payload;
        try { payload = call.local ? ArtActivityIntent.encodeLocal(data) : ArtActivityIntent.encode(data); }
        catch (IOException error) { throw new IllegalArgumentException("Cannot encode Activity result",error); }
        incoming.remove(token);
        execute(() -> {
            try { request("finish",call.route,Integer.toString(result),payload); }
            catch (IOException error) { Log.w("StoneActivities","Cross-process Activity result failed",error); }
        });
    }
    void removed(IBinder token) {
        finish(token,Activity.RESULT_CANCELED,null);
        for (String key : new java.util.ArrayList<>(outgoing.keySet())) if (outgoing.get(key).token == token) {
            outgoing.remove(key);
            execute(() -> { try { request("abandon",key); } catch (IOException error) { Log.w("StoneActivities","Activity cancellation failed",error); } });
        }
    }
    private void deliver(Outgoing call, int result, Intent intent) {
        if (call == null) return;
        if(call.local && outgoing.values().stream().noneMatch(other->other.local)) ArtWindow.setVisible(true);
        try { application.deliverExternalResult(call.token,call.who,call.request,result,intent); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot dispatch Activity result",error); }
    }
    private void poll() {
        try {
            String[] row = request("poll"); if (row.length == 0) return;
            if (row.length < 3) throw new IOException("Invalid Activity event");
            boolean local = row[1].equals("launch-local") || row[1].equals("result-local");
            boolean launch = row[1].equals("launch") || row[1].equals("launch-local");
            boolean result = row[1].equals("result") || row[1].equals("result-local");
            String payload = launch && row.length == 5 ? row[4] : result && row.length == 6 ? row[5] : "";
            Intent intent = local ? ArtActivityIntent.decodeLocal(payload,application.context.getClassLoader()) : ArtActivityIntent.decode(payload);
            String[] claim = request("claim",row[0]);
            if (claim.length != 1 || !claim[0].equals("1")) return;
            work.incrementAndGet();
            if (!main.post(() -> {
                try {
                    if (launch && row.length == 5) {
                        ActivityInfo info; ComponentName caller;
                        try {
                            if (intent == null || intent.getComponent() == null) throw new IllegalArgumentException("Cross-process Activity must be explicit");
                            info = application.apk.resolveActivity(intent);
                            if (local) {
                                accessibleLocal(info);
                                if (!ArtProcess.current(application.apk).equals(info.processName)) throw new SecurityException("Activity belongs to another application process");
                            } else accessible(info,application.context.getPackageName());
                            caller = row[3].isEmpty() ? null : ComponentName.unflattenFromString(row[3]);
                            if (!row[3].isEmpty() && caller == null) throw new IllegalArgumentException("Invalid Activity caller");
                        } catch (RuntimeException error) {
                            Log.e("StoneActivities","Activity launch rejected",error);
                            execute(() -> {
                                try { request("finish",row[2],"0",""); } catch (IOException failure) { Log.w("StoneActivities","Rejected Activity result failed",failure); }
                            });
                            return;
                        }
                        if (local) localRoutes.add(row[2]);
                        try { application.launchExternal(info,intent,row[2],caller); }
                        finally { localRoutes.remove(row[2]); }
                    } else if (result && row.length == 6) deliver(outgoing.remove(row[3]),Integer.parseInt(row[4]),intent);
                    else if (row[1].equals("revoke") && row.length == 3) {
                        for (Incoming call : incoming.values()) if (call.route.equals(row[2])) call.caller = null;
                    } else throw new IllegalArgumentException("Unknown Activity event");
                } catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot launch remote Activity",error); }
                finally { work.decrementAndGet(); }
            })) work.decrementAndGet();
            warned = false;
        } catch (IOException | RuntimeException error) {
            if (!warned) Log.w("StoneActivities","Activity route polling unavailable",error); warned = true;
        }
    }
}
