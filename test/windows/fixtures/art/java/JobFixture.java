package com.trebledot.stone.probe;
import android.app.Activity;
import android.app.Application;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.PersistableBundle;
import android.app.job.JobParameters;
import android.app.job.JobService;
import android.os.Looper;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
public final class JobFixture extends Activity {
    private static Context app;
    private static synchronized void event(String name) {
        if (app == null) return;
        try (java.io.FileWriter writer = new java.io.FileWriter(new java.io.File(app.getFilesDir(), "session-events.txt"), true)) {
            writer.write(name + "\n");
        } catch (java.io.IOException error) { throw new IllegalStateException(error); }
    }
    public static final class App extends Application {
        @Override public void onCreate() {
            super.onCreate(); app = this;
            java.io.File mode = new java.io.File(getFilesDir(), "session-mode.properties");
            if (!mode.isFile()) { app = null; return; }
            event("application-created"); java.util.Properties settings = new java.util.Properties();
            event("application-pid-" + android.os.Process.myPid());
            try (java.io.FileInputStream input = new java.io.FileInputStream(mode)) { settings.load(input); }
            catch (java.io.IOException error) { throw new IllegalStateException(error); }
            long duration = Long.parseLong(settings.getProperty("duration", "0"));
            java.io.File scheduled = new java.io.File(getFilesDir(), "session-scheduled.txt");
            boolean once = Boolean.parseBoolean(settings.getProperty("scheduleOnce", "false"));
            if (duration > 0 && (!once || !scheduled.exists())) {
                PersistableBundle extras = new PersistableBundle(); extras.putLong("duration", duration);
                int status = getSystemService(JobScheduler.class).schedule(new JobInfo.Builder(8001, new ComponentName(this, First.class))
                    .setExtras(extras).setMinimumLatency(Long.parseLong(settings.getProperty("latency", "100")))
                    .setPersisted(Boolean.parseBoolean(settings.getProperty("persisted", "false"))).build());
                if (status != JobScheduler.RESULT_SUCCESS) throw new IllegalStateException("Background fixture job was not scheduled");
                event("job-scheduled"); event("job-scheduled-at-" + System.currentTimeMillis());
                if (once) try (java.io.FileOutputStream output = new java.io.FileOutputStream(scheduled)) { output.write(1); output.getFD().sync(); }
                catch (java.io.IOException error) { throw new IllegalStateException(error); }
            }
        }
    }
    @Override public void onCreate(Bundle state) { super.onCreate(state); event("activity-created"); }
    @Override public void onDestroy() { event("activity-destroyed"); super.onDestroy(); }
    public static volatile int starts, stops, destroys, reason, marker;
    public static volatile boolean mainThread = true;
    public static final Map<String, JobParameters> active = new ConcurrentHashMap<>();
    private static final Map<String, JobService> owners = new ConcurrentHashMap<>();
    private static String key(JobParameters parameters) { return parameters.getJobNamespace() + ":" + parameters.getJobId(); }
    public static void finish(String key, boolean retry) { owners.get(key).jobFinished(active.get(key), retry); active.remove(key); owners.remove(key); }
    public static class First extends JobService {
        @Override public boolean onStartJob(JobParameters parameters) {
            mainThread &= Looper.myLooper() == Looper.getMainLooper(); starts++;
            marker = parameters.getExtras().getInt("marker", 0);
            if (!parameters.getExtras().getBoolean("async", true)) return false;
            active.put(key(parameters), parameters); owners.put(key(parameters), this);
            long duration = parameters.getExtras().getLong("duration", 0);
            if (duration > 0) {
                event("job-start-at-" + System.currentTimeMillis());
                event("job-start"); new Handler(Looper.getMainLooper()).postDelayed(() -> {
                    if (active.get(key(parameters)) != parameters) return;
                    event("job-finish"); finish(key(parameters), false);
                }, duration);
            }
            return true;
        }
        @Override public boolean onStopJob(JobParameters parameters) {
            mainThread &= Looper.myLooper() == Looper.getMainLooper(); stops++; reason = parameters.getStopReason();
            event("job-stop-" + reason);
            active.remove(key(parameters)); owners.remove(key(parameters)); return parameters.getExtras().getBoolean("retry", false);
        }
        @Override public void onDestroy() { destroys++; super.onDestroy(); }
    }
    public static final class Second extends First { }
    public static final class Invalid extends First { }
}
