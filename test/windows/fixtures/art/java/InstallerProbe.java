package com.trebledot.stone.probe;

import android.content.Context;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageInstaller.Session;
import android.content.pm.PackageInstaller.SessionInfo;
import android.content.pm.PackageInstaller.SessionParams;
import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Uses the image's public installer API, including its FileBridge write/fsync protocol. */
final class InstallerProbe {
    private static final byte[] CONTENT = {1, 2, 3, 4, 5, 6};
    private static final class Drained extends RuntimeException {}
    private static void drain() {
        Drained done = new Drained();
        new Handler(Looper.getMainLooper()).post(() -> { throw done; });
        try { Looper.loop(); }
        catch (Drained signal) { if (signal != done) throw signal; }
    }

    static void run(Context context) throws Exception {
        verifyDirectoryRemoval(context);
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        File recovery = new File(context.getFilesDir(), "installer-probe-session");
        if (recovery.isFile()) {
            byte[] bytes = new byte[32];
            int length;
            try (FileInputStream input = new FileInputStream(recovery)) { length = input.read(bytes); }
            int id = Integer.parseInt(new String(bytes, 0, length, "UTF-8"));
            SessionInfo restored = installer.getSessionInfo(id);
            if (restored == null || restored.isActive() || restored.isCommitted() || Math.abs(restored.getProgress() - 0.65f) > 0.001f
                    || !"更新标签".contentEquals(restored.getAppLabel()) || !"com.trebledot.stone.install.fixture".equals(restored.getAppPackageName()))
                throw new AssertionError("install-session metadata recovery");
            try (Session session = installer.openSession(id)) { verifyContent(session); }
            installer.abandonSession(id);
            if (!recovery.delete()) throw new AssertionError("install-session recovery marker cleanup");
            System.out.println("ART recovered install-session metadata and staged bytes from the previous process.");
        }
        verifyOwner(context, installer);
        List<String> events = new ArrayList<>();
        PackageInstaller.SessionCallback callback = new PackageInstaller.SessionCallback() {
            @Override public void onCreated(int id) { events.add("created:" + id); }
            @Override public void onBadgingChanged(int id) { events.add("badging:" + id); }
            @Override public void onActiveChanged(int id, boolean active) { events.add("active:" + id + ":" + active); }
            @Override public void onProgressChanged(int id, float progress) { events.add("progress:" + id); }
            @Override public void onFinished(int id, boolean success) { events.add("finished:" + id + ":" + success); }
        };
        installer.registerSessionCallback(callback, new Handler(Looper.getMainLooper()));
        SessionParams params = new SessionParams(SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName("com.trebledot.stone.install.fixture");
        params.setAppLabel("安装测试");
        params.setSize(CONTENT.length);
        Bitmap icon = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
        icon.eraseColor(0xffff0000);
        params.setAppIcon(icon);
        int id = installer.createSession(params);
        boolean preserved = false;
        try {
            icon.recycle();
            params.setAppLabel("Mutated caller parameters");
            SessionInfo info = installer.getSessionInfo(id);
            if (id <= 0 || info == null || info.isActive() || info.isCommitted() || !"安装测试".contentEquals(info.getAppLabel())
                    || info.getSize() != CONTENT.length || info.getAppIcon().getPixel(0, 0) != 0xffff0000
                    || !context.getPackageName().equals(info.getInstallerPackageName())
                    || installer.getMySessions().stream().noneMatch(entry -> entry.getSessionId() == id)
                    || installer.getAllSessions().stream().noneMatch(entry -> entry.getSessionId() == id))
                throw new AssertionError("install-session creation, metadata and parameter ownership");
            try (Session session = installer.openSession(id)) {
                if (!installer.getSessionInfo(id).isActive() || session.getNames().length != 0 || session.isMultiPackage() || session.isStaged())
                    throw new AssertionError("install-session active state");
                try (OutputStream output = session.openWrite("base.apk", 0, CONTENT.length)) {
                    output.write(CONTENT, 0, 3);
                    session.fsync(output);
                }
                try (OutputStream output = session.openWrite("base.apk", 3, 0)) {
                    output.write(CONTENT, 3, 3);
                    session.fsync(output);
                }
                verifyContent(session);
                session.setStagingProgress(0.4f);
                Session.class.getMethod("addProgress", float.class).invoke(session, 0.25f);
                verifyMetadataFailure(installer, session, id);
                try { session.setStagingProgress(Float.NaN); throw new AssertionError("NaN install progress accepted"); }
                catch (IllegalArgumentException expected) { }
                for (String name : new String[]{"../escape.apk", "x/y.apk", "x\\y.apk", ".", "..", "x:stream"}) {
                    try { session.openWrite(name, 0, -1); throw new AssertionError("staged path escape accepted"); }
                    catch (IllegalArgumentException | SecurityException expected) { }
                }
                try { session.openRead("absent.apk"); throw new AssertionError("missing staged file accepted"); }
                catch (java.io.IOException expected) { }
                try (Session second = installer.openSession(id)) { verifyContent(second); }
                if (!installer.getSessionInfo(id).isActive()) throw new AssertionError("nested session close changed active state");
            }
            if (installer.getSessionInfo(id).isActive() || Math.abs(installer.getSessionInfo(id).getProgress() - 0.65f) > 0.001f)
                throw new AssertionError("install-session close and progress");
            installer.updateSessionAppLabel(id, "更新标签");
            int cancelled = installer.createSession(new SessionParams(SessionParams.MODE_FULL_INSTALL));
            java.util.concurrent.atomic.AtomicReference<Throwable> bridgeFailure = new java.util.concurrent.atomic.AtomicReference<>();
            try (Session session = installer.openSession(cancelled)) {
                OutputStream output = session.openWrite("取消.apk", 0, -1);
                output.write(42);
                session.fsync(output);
                for (Thread thread : Thread.getAllStackTraces().keySet()) {
                    if (thread.getName().equals("Stone install session " + cancelled))
                        thread.setUncaughtExceptionHandler((failed, error) -> bridgeFailure.set(error));
                }
                session.abandon();
                try { output.write(43); throw new AssertionError("abandoned staging stream is still writable"); }
                catch (java.io.IOException expected) { }
                try { output.close(); } catch (java.io.IOException expected) { }
                try { session.getNames(); throw new AssertionError("abandoned session is still writable"); }
                catch (SecurityException expected) { }
            }
            long until = android.os.SystemClock.uptimeMillis() + 2000;
            while (bridgeRunning(cancelled) && android.os.SystemClock.uptimeMillis() < until) Thread.sleep(10);
            if (bridgeRunning(cancelled)) throw new AssertionError("abandoned FileBridge thread is still blocked");
            if (bridgeFailure.get() != null) throw new AssertionError("FileBridge cancellation raised an uncaught exception", bridgeFailure.get());
            if (installer.getSessionInfo(cancelled) != null || installer.getMySessions().stream().anyMatch(entry -> entry.getSessionId() == cancelled))
                throw new AssertionError("abandoned session remains visible");
            try { installer.openSession(cancelled); throw new AssertionError("abandoned session reopened"); }
            catch (SecurityException expected) { }
            drain();
            if (!events.contains("created:" + id) || !events.contains("active:" + id + ":true")
                    || !events.contains("active:" + id + ":false") || !events.contains("progress:" + id)
                    || !events.contains("badging:" + id) || !events.contains("finished:" + cancelled + ":false"))
                throw new AssertionError("install-session callbacks: " + events);
            try (FileOutputStream output = new FileOutputStream(recovery)) { output.write(Integer.toString(id).getBytes("UTF-8")); output.getFD().sync(); }
            preserved = true;
        } finally {
            if (!preserved) installer.abandonSession(id);
            installer.unregisterSessionCallback(callback);
        }
        System.out.println("ART staged install bytes through public PackageInstaller APIs, fsync, resume, callbacks, cancellation and ownership checks.");
    }

    private static void verifyContent(Session session) throws Exception {
        if (!Arrays.equals(session.getNames(), new String[]{"base.apk"})) throw new AssertionError("staged install file names");
        try (InputStream input = session.openRead("base.apk")) {
            ByteArrayOutputStream result = new ByteArrayOutputStream();
            byte[] bytes = new byte[16];
            int count;
            while ((count = input.read(bytes)) != -1) result.write(bytes, 0, count);
            if (!Arrays.equals(result.toByteArray(), CONTENT)) throw new AssertionError("staged install stream contents");
        }
    }

    private static void verifyOwner(Context context, PackageInstaller installer) throws Exception {
        Class<?> api = Class.forName("android.content.pm.IPackageInstaller");
        java.lang.reflect.Field field = PackageInstaller.class.getDeclaredField("mInstaller");
        field.setAccessible(true);
        Object backend = field.get(installer);
        int user = (Integer) Class.forName("android.os.UserHandle").getMethod("myUserId").invoke(null);
        for (PackageInstaller forged : new PackageInstaller[]{
                PackageInstaller.class.getConstructor(api, String.class, String.class, int.class).newInstance(backend, "other.installer", null, user),
                PackageInstaller.class.getConstructor(api, String.class, String.class, int.class).newInstance(backend, context.getPackageName(), null, user + 1)}) {
            try { forged.createSession(new SessionParams(SessionParams.MODE_FULL_INSTALL)); throw new AssertionError("installer identity accepted"); }
            catch (SecurityException expected) { }
        }
    }

    private static boolean bridgeRunning(int id) {
        for (Thread thread : Thread.getAllStackTraces().keySet()) {
            if (thread.isAlive() && thread.getName().equals("Stone install session " + id)) return true;
        }
        return false;
    }

    private static void verifyMetadataFailure(PackageInstaller installer, Session session, int id) throws Exception {
        File metadata = new File("/data/system/stone-package-installer/session-" + id, "session.properties");
        File saved = new File(metadata.getPath() + ".saved");
        android.system.Os.rename(metadata.getPath(), saved.getPath());
        try {
            if (!metadata.mkdir()) throw new AssertionError("install metadata failure fixture");
            try { session.setStagingProgress(0.9f); throw new AssertionError("failed session metadata replacement reported success"); }
            catch (RuntimeException expected) { if (!(expected.getCause() instanceof java.io.IOException)) throw expected; }
            try { installer.updateSessionAppLabel(id, "不可提交"); throw new AssertionError("failed session label replacement reported success"); }
            catch (RuntimeException expected) { if (!(expected.getCause() instanceof java.io.IOException)) throw expected; }
            if (Math.abs(installer.getSessionInfo(id).getProgress() - 0.65f) > 0.001f
                    || !"安装测试".contentEquals(installer.getSessionInfo(id).getAppLabel()))
                throw new AssertionError("failed metadata replacement changed session state");
        } finally {
            if (!metadata.delete()) throw new AssertionError("install metadata failure fixture cleanup");
            android.system.Os.rename(saved.getPath(), metadata.getPath());
        }
    }

    private static void verifyDirectoryRemoval(Context context) throws Exception {
        File directory = new File(context.getCacheDir(), "installer-delete-目录");
        if (!directory.mkdir()) throw new AssertionError("directory deletion fixture");
        File child = new File(directory, "暂存.apk");
        try (FileOutputStream output = new FileOutputStream(child)) { output.write(1); }
        if (directory.delete() || !child.isFile()) throw new AssertionError("nonempty directory removed");
        if (!child.delete() || !directory.delete() || directory.exists() || directory.delete())
            throw new AssertionError("file and empty directory deletion");
    }
}
