package com.trebledot.stone.runtime;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ComponentInfo;
import android.content.pm.ServiceInfo;
import android.net.Network;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.PersistableBundle;
import android.util.Base64;
import android.util.Log;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Service-owned job queue, with actual image JobService binding and callbacks in this ART. */
final class ArtJobs {
    private static native boolean available();
    private static native String[] exchange(String[] fields);
    private static native void removePackage(String name) throws java.io.IOException;
    static void packageRemoved(String name) throws java.io.IOException {
        if (available()) removePackage(name);
    }
    private final ArtPackage apk;
    private final boolean enabled = available();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ScheduledExecutorService worker = Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "Stone jobs"); thread.setDaemon(true); return thread;
    });
    private final Map<String, Run> runs = new ConcurrentHashMap<>();
    private final Map<String, Boolean> completions = new ConcurrentHashMap<>();
    private final AtomicInteger releases = new AtomicInteger();
    private volatile Context context;
    private boolean attached, warned;
    private boolean suspended;
    private volatile boolean shuttingDown;
    ArtJobs(ArtPackage apk) { this.apk = apk; }
    synchronized void attach(ArtApplication application) {
        context = application == null ? null : application.context;
        if (!enabled || attached || application == null || !ArtProcess.current(apk).equals(apk.applicationInfo.processName)) return;
        attached = true; worker.scheduleWithFixedDelay(this::poll, 1000, 250, TimeUnit.MILLISECONDS);
    }
    private String[] request(String... fields) {
        String[] all = new String[fields.length + 1]; all[0] = apk.applicationInfo.packageName;
        System.arraycopy(fields, 0, all, 1, fields.length); return exchange(all);
    }
    private static String namespace(Object value) {
        String name = value == null ? "" : (String) value;
        if (name.length() > 128 || name.indexOf('\0') >= 0) throw new IllegalArgumentException("Invalid job namespace");
        return name;
    }
    private static JobInfo decode(String blob) {
        byte[] bytes = Base64.decode(blob, Base64.NO_WRAP); Parcel parcel = Parcel.obtain();
        try { parcel.unmarshall(bytes, 0, bytes.length); parcel.setDataPosition(0); return JobInfo.CREATOR.createFromParcel(parcel); }
        finally { parcel.recycle(); }
    }
    private static String encode(JobInfo job) {
        Parcel parcel = Parcel.obtain();
        try {
            job.writeToParcel(parcel, 0);
            if (parcel.hasFileDescriptors()) throw new IllegalArgumentException("Job cannot retain file descriptors");
            String blob = Base64.encodeToString(parcel.marshall(), Base64.NO_WRAP);
            if (blob.length() > 24576) throw new IllegalArgumentException("Job payload is too large"); return blob;
        } finally { parcel.recycle(); }
    }
    private void validate(JobInfo job) {
        if (job == null || !apk.applicationInfo.packageName.equals(job.getService().getPackageName()))
            throw new IllegalArgumentException("Job belongs to another application");
        ComponentInfo component = apk.components.find(job.getService());
        if (!(component instanceof ServiceInfo) || !apk.components.enabled(component)
                || !apk.applicationInfo.processName.equals(component.processName)
                || (((ServiceInfo) component).flags & ServiceInfo.FLAG_ISOLATED_PROCESS) != 0)
            throw new IllegalArgumentException("JobService is unavailable in this process");
        if (!"android.permission.BIND_JOB_SERVICE".equals(((ServiceInfo) component).permission))
            throw new IllegalArgumentException("JobService must require BIND_JOB_SERVICE");
        if (job.isPersisted() && (apk.packageInfo.requestedPermissions == null
                || !java.util.Arrays.asList(apk.packageInfo.requestedPermissions).contains("android.permission.RECEIVE_BOOT_COMPLETED")))
            throw new SecurityException("Persisted job requires RECEIVE_BOOT_COMPLETED");
    }
    private int schedule(String name, JobInfo job) throws ReflectiveOperationException {
        validate(job);
        if (!enabled) return JobScheduler.RESULT_FAILURE; // Standalone probes have no Client job profile.
        if (job.getTriggerContentUris() != null && job.getTriggerContentUris().length != 0
                || job.getClipData() != null || (Boolean) JobInfo.class.getMethod("isUserInitiated").invoke(job)) {
            Log.w("StoneJobs", "Unsupported job " + job.getId() + ": idle=" + job.isRequireDeviceIdle() + " storage=" + job.isRequireStorageNotLow()
                + " triggers=" + (job.getTriggerContentUris() == null ? -1 : job.getTriggerContentUris().length)
                + " clip=" + (job.getClipData() != null) + " userInitiated=" + JobInfo.class.getMethod("isUserInitiated").invoke(job));
            return JobScheduler.RESULT_FAILURE;
        }
        int network = (Integer) JobInfo.class.getMethod("getNetworkType").invoke(job);
        int flags = (job.isRequireCharging() ? 1 : 0) | (job.isRequireBatteryNotLow() ? 2 : 0)
            | (network != JobInfo.NETWORK_TYPE_NONE ? 4 : 0) | (network == JobInfo.NETWORK_TYPE_UNMETERED ? 8 : 0)
            | (network == JobInfo.NETWORK_TYPE_METERED ? 16 : 0) | (job.isPersisted() ? 32 : 0)
            | (job.isRequireStorageNotLow() ? 64 : 0) | (job.isRequireDeviceIdle() ? 128 : 0);
        int warning = 15;
        int resource = apk.resources.getIdentifier("config_lowBatteryWarningLevel", "integer", "android");
        if (resource != 0) warning = apk.resources.getInteger(resource);
        int idle = config("config_jobSchedulerInactivityIdleThreshold", 1860000);
        int stableIdle = config("config_jobSchedulerInactivityIdleThresholdOnStablePower", idle);
        boolean late = (Boolean) JobInfo.class.getMethod("hasLateConstraint").invoke(job);
        long latency = job.isPeriodic() ? job.getIntervalMillis() - job.getFlexMillis() : job.getMinLatencyMillis();
        try {
            request("schedule", name, Integer.toString(job.getId()), encode(job), Long.toString(latency),
                late ? Long.toString(job.getMaxExecutionDelayMillis()) : "-1", Long.toString(job.isPeriodic() ? job.getIntervalMillis() : 0),
                Long.toString(job.getInitialBackoffMillis()), Integer.toString(job.getBackoffPolicy()), Integer.toString(flags), Integer.toString(warning),
                Integer.toString(Math.max(0, idle)), Integer.toString(Math.max(0, stableIdle)));
            return JobScheduler.RESULT_SUCCESS;
        } catch (RuntimeException error) { Log.w("StoneJobs", "Cannot save job", error); return JobScheduler.RESULT_FAILURE; }
    }
    private static int config(String name, int fallback) {
        android.content.res.Resources resources = android.content.res.Resources.getSystem();
        int id = resources.getIdentifier(name, "integer", "android"); return id == 0 ? fallback : resources.getInteger(id);
    }
    private Map<String, List<JobInfo>> pending() {
        if (!enabled) return new LinkedHashMap<>();
        for (int attempt = 0; attempt < 3; ++attempt) {
            Map<String, List<JobInfo>> result = new LinkedHashMap<>();
            int offset = 0; String revision = null;
            for (;;) {
                String[] row = revision == null ? request("list", "0") : request("list", Integer.toString(offset), revision);
                if (row.length == 1 && row[0].equals("changed")) break;
                if (row.length == 1 && row[0].equals("end")) return result;
                if (row.length != 6 || !row[0].equals("item")) throw new IllegalStateException("Invalid job listing");
                revision = row[1]; offset = Integer.parseInt(row[2]);
                result.computeIfAbsent(row[3].isEmpty() ? null : row[3], unused -> new ArrayList<>()).add(decode(row[5]));
            }
        }
        throw new IllegalStateException("Job listing changed repeatedly");
    }
    Object call(Object instance, Method method, Object[] args) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "schedule": return schedule(namespace(args[0]), (JobInfo) args[1]);
            case "scheduleAsPackage":
                if (!apk.applicationInfo.packageName.equals(args[2]) || (Integer) args[3] != ((Number) Class.forName("android.os.UserHandle").getMethod("myUserId").invoke(null)).intValue())
                    throw new SecurityException("Cross-application scheduling is unavailable");
                return schedule(namespace(args[0]), (JobInfo) args[1]);
            case "enqueue": return JobScheduler.RESULT_FAILURE; // Work-item queues require an additional ownership protocol.
            case "cancel": if (enabled) request("cancel", namespace(args[0]), args[1].toString()); return null;
            case "cancelAll": if (enabled) request("cancel-all"); return null;
            case "cancelAllInNamespace": if (enabled) request("cancel-ns", namespace(args[0])); return null;
            case "getPendingJob": {
                if (!enabled) return null; String[] row = request("get", namespace(args[0]), args[1].toString());
                return row.length == 0 ? null : decode(row[0]);
            }
            case "getAllPendingJobs": {
                Map<String, Object> slices = new LinkedHashMap<>();
                Class<?> slice = Class.forName("android.content.pm.ParceledListSlice");
                for (Map.Entry<String, List<JobInfo>> entry : pending().entrySet()) slices.put(entry.getKey(), slice.getConstructor(List.class).newInstance(entry.getValue()));
                return slices;
            }
            case "getAllPendingJobsInNamespace":
                return method.getReturnType().getConstructor(List.class).newInstance(pending().getOrDefault(args[0], Collections.emptyList()));
            case "canRunUserInitiatedJobs":
            case "hasRunUserInitiatedJobsPermission": return false;
            default: throw new UnsupportedOperationException("ART job service: " + method.getName());
        }
    }
    synchronized boolean retireIfIdle() {
        if (!idle()) return false;
        suspended = ArtSession.retire(); return suspended;
    }
    boolean idle() { return runs.isEmpty() && completions.isEmpty() && releases.get() == 0; }
    void stopForShutdown() {
        shuttingDown = true;
        for (Run run : runs.values()) stop(run, 3);
    }
    private synchronized void poll() {
        if (suspended) return;
        try {
            while (releases.get() > 0) { ArtServices.power.releaseJob(); releases.decrementAndGet(); }
            for (Map.Entry<String, Boolean> completion : completions.entrySet()) {
                request("finish", completion.getKey(), completion.getValue() ? "1" : "0"); completions.remove(completion.getKey(), completion.getValue());
            }
            if (shuttingDown) return;
            String[] command = request("poll");
            if (command.length == 0) return;
            if (command[0].equals("start") && command.length == 6) {
                if (shuttingDown) { completions.put(command[1], true); return; }
                if (completions.containsKey(command[1])) return;
                if (runs.containsKey(command[1])) { request("accept", command[1]); return; }
                try {
                    Run run = new Run(command[1], command[2], decode(command[4]), command[5].equals("1"));
                    for (Run previous : runs.values()) if (previous.name.equals(run.name) && previous.job.getId() == run.job.getId()) {
                        completions.put(run.lease, true); return;
                    }
                    ArtServices.power.holdJob(); run.power = true; runs.put(run.lease, run);
                    main.post(() -> { if (shuttingDown) complete(run, true); else bind(run); });
                } catch (RuntimeException error) {
                    completions.put(command[1], true); throw error;
                }
                request("accept", command[1]);
            } else if (command[0].equals("stop") && command.length == 3) {
                Run run = runs.get(command[1]);
                if (run != null) main.post(() -> stop(run, Integer.parseInt(command[2])));
                else completions.putIfAbsent(command[1], !command[2].equals("1"));
                request("stop-accept", command[1]);
            } else throw new IllegalStateException("Invalid job dispatch");
            warned = false;
        } catch (Exception error) { if (!warned) Log.w("StoneJobs", "Job dispatch unavailable", error); warned = true; }
    }
    private final class Run implements ServiceConnection {
        final String lease, name; final JobInfo job; final boolean expired;
        JobParameters parameters; Object service; boolean bound, started, stopSent, power; volatile boolean done; int stop = -1;
        Run(String lease, String name, JobInfo job, boolean expired) { this.lease = lease; this.name = name; this.job = job; this.expired = expired; }
        @Override public void onServiceConnected(ComponentName component, IBinder binder) {
            if (done) return;
            try {
                if (binder == null) throw new IllegalStateException("JobService returned no Binder");
                service = Class.forName("android.app.job.IJobService$Stub").getMethod("asInterface", IBinder.class).invoke(null, binder);
                if (stop >= 0) { complete(this, stop != 1); return; }
                Binder callback = ArtServices.service("android.app.job.IJobCallback", (instance, method, args) -> callback(this, method, args));
                Class<?> type = JobParameters.class;
                Network network = job.getRequiredNetwork() == null ? null
                    : context.getSystemService(android.net.ConnectivityManager.class).getActiveNetwork();
                parameters = (JobParameters) type.getConstructor(IBinder.class, String.class, int.class, PersistableBundle.class, Bundle.class,
                    android.content.ClipData.class, int.class, boolean.class, boolean.class, boolean.class, android.net.Uri[].class, String[].class, Network.class)
                    .newInstance(callback, name.isEmpty() ? null : name, job.getId(), new PersistableBundle(job.getExtras()), new Bundle(job.getTransientExtras()),
                        null, 0, expired, job.isExpedited(), false, null, null, network);
                started = true; invoke("startJob", this);
            } catch (Exception error) { Log.w("StoneJobs", "Cannot start JobService", error); complete(this, true); }
        }
        @Override public void onServiceDisconnected(ComponentName name) { complete(this, true); }
        @Override public void onBindingDied(ComponentName name) { complete(this, true); }
        @Override public void onNullBinding(ComponentName name) { complete(this, true); }
    }
    private void bind(Run run) {
        if (run.done) return;
        if (run.stop >= 0) { complete(run, run.stop != 1); return; }
        try {
            run.bound = context.bindService(new Intent().setComponent(run.job.getService()), run, Context.BIND_AUTO_CREATE);
            if (!run.bound) throw new IllegalStateException("Cannot bind JobService");
        } catch (Exception error) { Log.w("StoneJobs", "JobService binding failed", error); complete(run, true); }
    }
    private Object callback(Run run, Method method, Object[] args) {
        if (run.done) return method.getReturnType() == boolean.class ? false : null;
        if (args == null || !(args[0] instanceof Integer) || (Integer) args[0] != run.job.getId()) throw new SecurityException("Job callback ID mismatch");
        switch (method.getName()) {
            case "acknowledgeStartMessage": if (!(Boolean) args[1]) main.post(() -> complete(run, false)); return null;
            case "acknowledgeStopMessage":
            case "jobFinished": main.post(() -> complete(run, (Boolean) args[1])); return null;
            case "dequeueWork": return null;
            case "handleAbandonedJob": main.post(() -> complete(run, true)); return null;
            case "updateEstimatedNetworkBytes":
            case "updateTransferredNetworkBytes":
            case "acknowledgeGetTransferredDownloadBytesMessage":
            case "acknowledgeGetTransferredUploadBytesMessage": return null;
            default: throw new UnsupportedOperationException("ART job callback: " + method.getName());
        }
    }
    private void invoke(String operation, Run run) throws ReflectiveOperationException {
        Class.forName("android.app.job.IJobService").getMethod(operation, JobParameters.class).invoke(run.service, run.parameters);
    }
    private void stop(Run run, int reason) {
        if (run.done) return; run.stop = reason;
        if (!run.started || run.stopSent) return;
        run.stopSent = true;
        try {
            JobParameters.class.getMethod("setStopReason", int.class, int.class, String.class).invoke(run.parameters, reason, 0, "Stone job constraint or cancellation");
            invoke("stopJob", run);
        } catch (Exception error) { Log.w("StoneJobs", "Cannot stop JobService", error); complete(run, reason != 1); }
    }
    private void complete(Run run, boolean retry) {
        if (run.done) return; run.done = true; completions.put(run.lease, retry); runs.remove(run.lease, run);
        if (run.bound) { try { context.unbindService(run); } catch (RuntimeException error) { Log.w("StoneJobs", "Cannot unbind JobService", error); } }
        if (run.power) { run.power = false; releases.incrementAndGet(); }
    }
}
