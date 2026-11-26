package com.trebledot.stone.probe;

import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.VelocityTracker;
import java.io.FileDescriptor;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class Framework {
    private static void resolveLoopbacks() throws Exception {
        // Call the image's resolver explicitly: InetAddress's numeric fast path
        // would otherwise bypass libjavacore's android_getaddrinfofornet import.
        Class<?> hintsType = Class.forName("android.system.StructAddrinfo");
        java.lang.reflect.Method resolve = Os.class.getDeclaredMethod("android_getaddrinfo",
                String.class, hintsType, int.class);
        resolve.setAccessible(true);
        for (String host : new String[]{"127.0.0.1", "::1", "localhost"}) {
            Object hints = hintsType.getConstructor().newInstance();
            hintsType.getField("ai_socktype").setInt(hints, OsConstants.SOCK_STREAM);
            if (!host.equals("localhost")) hintsType.getField("ai_flags").setInt(hints, OsConstants.AI_NUMERICHOST);
            InetAddress[] addresses = (InetAddress[]) resolve.invoke(null, host, hints, 0);
            if (addresses == null || addresses.length == 0) throw new AssertionError("empty resolver result " + host);
            for (InetAddress address : addresses) {
                if (!address.isLoopbackAddress()) throw new AssertionError("non-loopback resolver result " + address);
                if (host.equals("127.0.0.1") && address.getAddress().length != 4
                        || host.equals("::1") && address.getAddress().length != 16)
                    throw new AssertionError("resolver address family " + host);
            }
        }
    }

    public static int networkResolver() throws Exception {
        resolveLoopbacks();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try { resolveLoopbacks(); } catch (Throwable error) { failure.set(error); }
        }, "stone-image-dns");
        worker.setDaemon(true);
        worker.start();
        worker.join(5000);
        if (worker.isAlive()) throw new AssertionError("image resolver worker did not finish");
        if (failure.get() != null) throw new AssertionError("image resolver worker failed", failure.get());
        System.out.println("PASS image Os.android_getaddrinfo IPv4/IPv6/localhost on main and managed thread.");
        return 42;
    }

    public static int selfSignals() {
        int pid = android.os.Process.myPid();
        int tid = android.os.Process.myTid();
        int priority = android.os.Process.getThreadPriority(tid);
        android.os.Process.sendSignal(pid, 0);
        try {
            android.os.Process.sendSignal(pid == 1 ? 2 : 1, 0);
            throw new AssertionError("foreign process signal accepted");
        } catch (SecurityException expected) { }
        for (int signal : new int[]{-1, 65}) {
            try {
                android.os.Process.sendSignal(pid, signal);
                throw new AssertionError("invalid process signal accepted");
            } catch (IllegalArgumentException expected) { }
        }
        try {
            android.os.Process.sendSignal(pid, 15);
            throw new AssertionError("unsupported process signal accepted");
        } catch (UnsupportedOperationException expected) { }
        if (android.os.Process.getThreadPriority(tid) != priority)
            throw new AssertionError("process signal probe changed thread priority");
        System.out.println("PASS image Process.sendSignal self probe and target/signal guards.");
        if (Boolean.getBoolean("stone.probe.self-kill")) {
            System.out.println("Requesting image Process.killProcess for this isolated probe pid=" + pid);
            System.out.flush();
            android.os.Process.killProcess(pid);
            throw new AssertionError("SIGKILL returned to Java");
        }
        return 42;
    }

    public static int threadPriorities() throws Exception {
        int tid = android.os.Process.myTid();
        int original = android.os.Process.getThreadPriority(tid);
        boolean changed = false;
        try {
            for (int priority : new int[]{-2, -1, 0, 5}) {
                android.os.Process.setThreadPriority(priority);
                changed = true;
                if (android.os.Process.getThreadPriority(tid) != priority)
                    throw new AssertionError("image Process thread priority " + priority);
            }
        } finally {
            if (changed) android.os.Process.setThreadPriority(original);
        }
        try {
            android.os.Process.getThreadPriority(-1);
            throw new AssertionError("unknown image Process thread accepted");
        } catch (IllegalArgumentException expected) { }
        for (final int priority : new int[]{-2, 0}) {
            final AtomicReference<Throwable> failure = new AtomicReference<>();
            HandlerThread worker = new HandlerThread("stone-priority-" + priority, priority) {
                @Override protected void onLooperPrepared() {
                    try {
                        if (android.os.Process.getThreadPriority(android.os.Process.myTid()) != priority)
                            throw new AssertionError("image HandlerThread priority " + priority);
                    } catch (Throwable error) { failure.set(error); }
                    finally { getLooper().quit(); }
                }
            };
            worker.setDaemon(true);
            worker.setUncaughtExceptionHandler((thread, error) -> failure.set(error));
            worker.start();
            worker.join(5000);
            if (worker.isAlive()) {
                worker.quit();
                throw new AssertionError("image HandlerThread did not finish");
            }
            if (failure.get() != null) throw new AssertionError("image HandlerThread failed", failure.get());
        }
        System.out.println("PASS image Process priorities and HandlerThread -2/0 startup, Looper and exit.");
        return 42;
    }

    public static int inputEvents() throws Exception {
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[2];
        MotionEvent.PointerCoords[] coordinates = new MotionEvent.PointerCoords[2];
        for (int i = 0; i < 2; i++) {
            properties[i] = new MotionEvent.PointerProperties();
            coordinates[i] = new MotionEvent.PointerCoords();
            properties[i].id = i == 0 ? 0 : 7;
            properties[i].toolType = i == 0 ? MotionEvent.TOOL_TYPE_MOUSE : MotionEvent.TOOL_TYPE_STYLUS;
            coordinates[i].x = i == 0 ? 12.5f : -3.5f;
            coordinates[i].y = i == 0 ? 18.25f : 42.75f;
            coordinates[i].pressure = 0.75f;
        }
        MotionEvent event = MotionEvent.obtain(0, 16, MotionEvent.ACTION_POINTER_DOWN | (1 << 8), 2,
                properties, coordinates, KeyEvent.META_SHIFT_ON, MotionEvent.BUTTON_PRIMARY,
                1, 1, -1, 0, InputDevice.SOURCE_MOUSE, 0);
        try {
            if (event.getPointerCount() != 2 || event.getActionMasked() != MotionEvent.ACTION_POINTER_DOWN
                    || event.getActionIndex() != 1 || event.getPointerId(1) != 7 || event.findPointerIndex(7) != 1
                    || event.findPointerIndex(99) != -1 || event.getToolType(0) != MotionEvent.TOOL_TYPE_MOUSE
                    || event.getToolType(1) != MotionEvent.TOOL_TYPE_STYLUS || event.getX(0) != 12.5f
                    || event.getY(1) != 42.75f || event.getPressure(1) != 0.75f
                    || event.getEventTime() != 16 || event.getButtonState() != MotionEvent.BUTTON_PRIMARY
                    || event.getSource() != InputDevice.SOURCE_MOUSE || event.getMetaState() != KeyEvent.META_SHIFT_ON)
                throw new AssertionError("image MotionEvent pointer fields");
            MotionEvent copy = MotionEvent.obtain(event);
            try {
                copy.offsetLocation(2, -4);
                copy.setAction(MotionEvent.ACTION_CANCEL);
                if (copy.getX(0) != 14.5f || copy.getY(1) != 38.75f || copy.getActionMasked() != MotionEvent.ACTION_CANCEL
                        || event.getX(0) != 12.5f || event.getY(1) != 42.75f)
                    throw new AssertionError("image MotionEvent copy, offset and cancellation");
            } finally { copy.recycle(); }
        } finally { event.recycle(); }
        // Select the image's LSQ2 strategy explicitly; the default selection asks
        // InputManagerGlobal, which belongs to the full application host.
        java.lang.reflect.Field strategy = VelocityTracker.class.getDeclaredField("VELOCITY_TRACKER_STRATEGY_LSQ2");
        strategy.setAccessible(true);
        java.lang.reflect.Method obtain = VelocityTracker.class.getDeclaredMethod("obtain", int.class);
        obtain.setAccessible(true);
        VelocityTracker tracker = (VelocityTracker) obtain.invoke(null, strategy.getInt(null));
        MotionEvent down = MotionEvent.obtain(0, 0, MotionEvent.ACTION_DOWN, 0, 0, 0);
        MotionEvent move = MotionEvent.obtain(0, 10, MotionEvent.ACTION_MOVE, 10, 20, 0);
        try {
            move.addBatch(20, 20, 40, 1, 1, 0);
            if (move.getHistorySize() != 1 || move.getHistoricalEventTime(0) != 10
                    || move.getHistoricalX(0) != 10 || move.getHistoricalY(0) != 20
                    || move.getX() != 20 || move.getY() != 40 || move.getEventTime() != 20)
                throw new AssertionError("image MotionEvent batched history");
            tracker.addMovement(down);
            tracker.addMovement(move);
            tracker.computeCurrentVelocity(1000);
            float x = tracker.getXVelocity(), y = tracker.getYVelocity();
            if (!Float.isFinite(x) || !Float.isFinite(y) || x <= 0 || y <= x)
                throw new AssertionError("image VelocityTracker: " + x + "," + y);
        } finally { down.recycle(); move.recycle(); tracker.recycle(); }
        KeyEvent key = new KeyEvent(0, 5, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A, 2,
                KeyEvent.META_SHIFT_ON, -1, 0, 0, InputDevice.SOURCE_KEYBOARD);
        if (key.getAction() != KeyEvent.ACTION_DOWN || key.getKeyCode() != KeyEvent.KEYCODE_A
                || key.getRepeatCount() != 2 || !key.isShiftPressed() || key.getEventTime() != 5
                || key.getSource() != InputDevice.SOURCE_KEYBOARD)
            throw new AssertionError("image KeyEvent fields");
        System.out.println("PASS image MotionEvent pointers/copy/offset/cancel/history, VelocityTracker and KeyEvent fields.");
        return 42;
    }

    private static final class Node { int value = 42; }
    private static int fieldValue(Node value) { return value.value; }
    private static String virtualValue(Object value) { return value.toString(); }

    private static void checkNetworkErrors() throws Exception {
        if (!"Connection refused".equals(Os.strerror(OsConstants.ECONNREFUSED))
                || !"Unknown error -1".equals(Os.strerror(-1)))
            throw new AssertionError("Android strerror numbering");
        InetAddress loopback = InetAddress.getByName("127.0.0.1");
        // A bound, non-listening socket reserves the port without accepting a
        // connection. There is no race to reuse an already closed test port.
        FileDescriptor reserved = Os.socket(OsConstants.AF_INET, OsConstants.SOCK_STREAM, 0);
        try {
            Os.bind(reserved, loopback, 0);
            int port = ((InetSocketAddress) Os.getsockname(reserved)).getPort();
            FileDescriptor client = Os.socket(OsConstants.AF_INET, OsConstants.SOCK_STREAM, 0);
            try {
                try { Os.connect(client, loopback, port); throw new AssertionError("Connected to a non-listening socket"); }
                catch (ErrnoException expected) {
                    if (expected.errno != OsConstants.ECONNREFUSED
                            || !expected.getMessage().contains("ECONNREFUSED (Connection refused)"))
                        throw new AssertionError("Socket failure did not retain Android errno and text", expected);
                }
            } finally { Os.close(client); }
        } finally { Os.close(reserved); }
        System.out.println("PASS image Android Os.strerror and actual refused TCP connection: ECONNREFUSED (Connection refused).");
    }

    public static int run() throws Exception {
        Node node = new Node();
        for (int index = 0; index < 32; index++) {
            if (fieldValue(node) != 42 || virtualValue(node) == null) throw new AssertionError("interpreter receiver access");
        }
        try { fieldValue(null); throw new AssertionError("implicit field null check"); }
        catch (NullPointerException expected) { }
        try { virtualValue(null); throw new AssertionError("implicit virtual call null check"); }
        catch (NullPointerException expected) { }
        if (Build.VERSION.SDK_INT != 37) throw new AssertionError("image system properties");
        checkNetworkErrors();
        if (Build.SUPPORTED_ABIS.length != 1 || !"x86_64".equals(Build.SUPPORTED_ABIS[0]))
            throw new AssertionError("ART native ABI");
        long start = SystemClock.uptimeMillis();
        int uid = Binder.getCallingUid();
        long token = Binder.clearCallingIdentity();
        if (Binder.getCallingUid() != uid) throw new AssertionError("local Binder identity");
        Binder.restoreCallingIdentity(token);
        final AtomicInteger result = new AtomicInteger();
        final AtomicReference<Handler> queue = new AtomicReference<Handler>();
        final CountDownLatch ready = new CountDownLatch(1);
        Thread worker = new Thread(new Runnable() {
            @Override public void run() {
                Looper.prepare();
                final Looper looper = Looper.myLooper();
                queue.set(new Handler(looper));
                ready.countDown();
                Looper.loop();
            }
        }, "stone-framework-looper");
        worker.setDaemon(true);
        worker.start();
        if (!ready.await(2, TimeUnit.SECONDS)) throw new AssertionError("Looper setup");
        Thread.sleep(25);
        final long posted = SystemClock.uptimeMillis();
        if (!queue.get().postDelayed(new Runnable() {
            @Override public void run() {
                if (SystemClock.uptimeMillis() - posted >= 50) result.set(42);
                Looper.myLooper().quit();
            }
        }, 50)) throw new AssertionError("Handler enqueue");
        worker.join(5000);
        if (worker.isAlive() || result.get() != 42) throw new AssertionError("Handler dispatch");
        if (SystemClock.uptimeMillis() < start) throw new AssertionError("uptime clock");
        return result.get();
    }
}
