package com.trebledot.stone.probe;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.content.res.AssetManager;
import android.os.PersistableBundle;
import android.os.SystemClock;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.util.Map;
import java.util.function.BooleanSupplier;
public final class JobSample {
    private static native int power();
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static void await(BooleanSupplier ready) throws Exception {
        long end = SystemClock.uptimeMillis() + 20000;
        while (!ready.getAsBoolean()) { check(SystemClock.uptimeMillis() < end, "Job callback timed out"); BroadcastProbe.drain(); Thread.sleep(5); }
    }
    private static Class<?> fixture;
    private static int value(String field) throws Exception { return fixture.getField(field).getInt(null); }
    @SuppressWarnings("unchecked") private static Map<String, Object> active() throws Exception { return (Map<String, Object>) fixture.getField("active").get(null); }
    private static JobInfo.Builder job(Context context, int id, int marker) {
        PersistableBundle extras = new PersistableBundle(); extras.putInt("marker", marker); extras.putBoolean("async", true);
        return new JobInfo.Builder(id, new ComponentName(context.getPackageName(), "com.trebledot.stone.probe.JobFixture$First"))
            .setExtras(extras).setMinimumLatency(1);
    }
    private static void finish(String key, boolean retry) throws Exception { fixture.getMethod("finish", String.class, boolean.class).invoke(null, key, retry); }
    public static int run(AssetManager assets, String stage) throws Exception {
        ArtPackage apk = ArtPackage.openApplication(assets, "/data/application.apk");
        ArtApplication running = ArtApplication.start(apk); Context context = running.context;
        JobScheduler scheduler = context.getSystemService(JobScheduler.class);
        check(context.getPackageName().equals("com.trebledot.stone.job.fixture"), "Job fixture required");
        fixture = Class.forName("com.trebledot.stone.probe.JobFixture");
        JobScheduler persisted = scheduler.forNamespace("persist");
        if (stage.equals("recover")) {
            JobInfo saved = persisted.getPendingJob(9001);
            check(saved != null && saved.getExtras().getInt("marker") == 91 && saved.isPersisted(), "Persisted job did not survive Service replacement");
            check(persisted.getPendingJob(9002) == null, "Non-persisted job survived Service replacement");
            System.out.println("PASS persisted JobInfo/namespace/extras recovered in a new Service; non-persisted jobs dropped"); return 42;
        }
        check(stage.equals("fixture"), "Unknown job fixture stage");
        scheduler.cancelInAllNamespaces();
        check(scheduler.schedule(job(context, 1, 11).setMinimumLatency(200).build()) == JobScheduler.RESULT_SUCCESS, "Job submission failed");
        JobInfo copy = scheduler.getPendingJob(1); check(copy != null && copy.getExtras().getInt("marker") == 11, "Pending job missing");
        copy.getExtras().putInt("marker", 99); check(scheduler.getPendingJob(1).getExtras().getInt("marker") == 11, "Pending job was not copied");
        await(() -> { try { return active().containsKey("null:1"); } catch (Exception error) { throw new RuntimeException(error); } });
        await(() -> power() == 1); int beforeStops = value("stops");
        scheduler.cancel(1);
        await(() -> { try { return value("stops") > beforeStops; } catch (Exception error) { throw new RuntimeException(error); } });
        check(value("reason") == 1 && scheduler.getPendingJob(1) == null, "Cancellation reason or pending state");
        await(() -> power() == 0);
        JobScheduler other = scheduler.forNamespace("other");
        check(scheduler.schedule(job(context, 2, 20).build()) == 1 && other.schedule(job(context, 2, 21).build()) == 1, "Namespaced jobs failed");
        await(() -> { try { return active().containsKey("null:2") && active().containsKey("other:2"); } catch (Exception error) { throw new RuntimeException(error); } });
        scheduler.cancelAll(); await(() -> scheduler.getPendingJob(2) == null);
        check(other.getPendingJob(2) != null, "Namespace cancellation affected another namespace");
        finish("other:2", false); await(() -> other.getPendingJob(2) == null); await(() -> power() == 0);
        check(scheduler.schedule(job(context, 3, 30).build()) == 1, "Replacement initial job failed");
        await(() -> { try { return active().containsKey("null:3"); } catch (Exception error) { throw new RuntimeException(error); } });
        check(scheduler.schedule(job(context, 3, 31).build()) == 1, "Replacement submission failed");
        await(() -> { try { return value("marker") == 31 && active().containsKey("null:3"); } catch (Exception error) { throw new RuntimeException(error); } });
        finish("null:3", false); await(() -> scheduler.getPendingJob(3) == null); await(() -> power() == 0);
        JobInfo.Builder sync = job(context, 4, 40); PersistableBundle immediate = new PersistableBundle(); immediate.putBoolean("async", false); sync.setExtras(immediate);
        check(scheduler.schedule(sync.build()) == 1, "Synchronous job failed"); await(() -> scheduler.getPendingJob(4) == null); await(() -> power() == 0);
        boolean rejected = false;
        try { scheduler.schedule(new JobInfo.Builder(5, new ComponentName(context.getPackageName(), "com.trebledot.stone.probe.JobFixture$Invalid")).setMinimumLatency(1).build()); }
        catch (IllegalArgumentException expected) { rejected = true; }
        check(rejected, "Unprotected JobService accepted");
        check((Boolean) fixture.getField("mainThread").get(null), "Lifecycle ran off main Looper");
        check(persisted.schedule(job(context, 9001, 91).setPersisted(true).setMinimumLatency(3600000).build()) == 1, "Persisted job failed");
        check(persisted.schedule(job(context, 9002, 92).setMinimumLatency(3600000).build()) == 1, "Non-persisted comparison job failed");
        System.out.println("PASS real JobService binding/start/stop/finish, namespaces, replacement, parcel copy, main thread and host power without WAKE_LOCK permission");
        return 42;
    }
}
