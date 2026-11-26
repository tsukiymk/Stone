package com.trebledot.stone.probe;

import java.util.HashMap;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.CRC32;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.security.MessageDigest;

public final class Main {
    private static final class NullReceiver { int value = 42; }
    private static int fieldValue(NullReceiver value) { return value.value; }
    private static String virtualValue(Object value) { return value.toString(); }

    private static native int twice(int value);
    private static native boolean jitReady();

    private static int hot(int seed) {
        if (seed < 0) throw new IllegalArgumentException("jit negative");
        int value = seed;
        for (int i = 0; i < 16; ++i) value = Integer.rotateLeft(value ^ (i * 31 + 7),3) + twice(i);
        return value;
    }

    private static int switchHot(int seed) {
        switch (seed) {
            case 0: return twice(2); case 1: return twice(3); case 2: return twice(5); case 3: return twice(7);
            case 4: return twice(11); case 5: return twice(13); case 6: return twice(17); case 7: return twice(19);
            case 8: return twice(23); case 9: return twice(29); case 10: return twice(31); case 11: return twice(37);
            case 12: return twice(41); case 13: return twice(43); case 14: return twice(47); case 15: return twice(53);
            default: return twice(59);
        }
    }

    private static int catchHot(int divisor) {
        try { return twice(42) / divisor; }
        catch (ArithmeticException error) { return twice(21); }
    }

    private static void verifyJit() throws Exception {
        final int[] expected = {-154962802,-155421554,-155356018,-155290482,
            -155224946,-155683698,-155618162,-155552626,-176458610,-176917362,
            -178948978,-178883442,-178817906,-179276658,-179211122,-179145586};
        long deadline = System.nanoTime() + 10_000_000_000L;
        final int[] switched = {4,6,10,14,22,26,34,38,46,58,62,74,82,86,94,106};
        do {
            for (int i = 0; i < 4096; ++i) if (hot(i & 15) != expected[i & 15] || switchHot(i & 15) != switched[i & 15] || catchHot(2) != 42)
                throw new AssertionError("JIT warmup result");
        } while (!jitReady() && System.nanoTime() < deadline);
        if (!jitReady()) throw new AssertionError("Expected probe method was not JIT compiled");
        if (catchHot(0) != 42 || catchHot(-2) != -42) throw new AssertionError("Compiled catch handler");
        Thread[] workers = new Thread[2];
        final AtomicInteger checked = new AtomicInteger();
        for (int worker = 0; worker < workers.length; ++worker) {
            workers[worker] = new Thread(() -> {
                for (int i = 0; i < 4096; ++i) if (hot(i & 15) != expected[i & 15]) return;
                checked.incrementAndGet();
            },"stone-jit-check-" + worker);
            workers[worker].start();
        }
        System.gc();
        for (Thread worker : workers) { worker.join(5000); if (worker.isAlive()) throw new AssertionError("JIT worker timeout"); }
        if (checked.get() != workers.length) throw new AssertionError("Compiled method across managed threads and GC");
        try { hot(-1); throw new AssertionError("Compiled exception was not thrown"); }
        catch (IllegalArgumentException expectedError) {
            if (!"jit negative".equals(expectedError.getMessage())) throw new AssertionError("Compiled exception message");
        }
        for (int i = 0; i < 4096; ++i) if (hot(i & 15) != expected[i & 15] || switchHot(i & 15) != switched[i & 15])
            throw new AssertionError("JIT result after GC/exception");
        if (switchHot(-1) != 118 || switchHot(16) != 118) throw new AssertionError("JIT switch default");
        if (catchHot(0) != 42 || catchHot(-2) != -42) throw new AssertionError("Compiled catch handler after GC");
        System.out.println("PASS compiled Java method, JNI callbacks, two managed threads, GC and exception recovery.");
    }

    public static int sum(int[] values) {
        int result = 0;
        for (int value : values) result += value;
        return result;
    }

    public static double fraction() { return 42.5; }

    private static long storageBlocks(String path) throws android.system.ErrnoException {
        android.system.StructStatVfs space = android.system.Os.statvfs(path);
        if (space.f_bsize <= 0 || space.f_frsize <= 0 || space.f_blocks <= 0
                || space.f_bfree < 0 || space.f_bfree > space.f_blocks
                || space.f_bavail < 0 || space.f_bavail > space.f_bfree || space.f_namemax <= 0)
            throw new AssertionError("image filesystem space fields");
        return space.f_blocks;
    }

    private static void verifyStorage(String path) throws Exception {
        long blocks = storageBlocks(path);
        final long[] observed = {-1};
        Thread worker = new Thread(() -> {
            try { observed[0] = storageBlocks(path); }
            catch (android.system.ErrnoException error) { throw new AssertionError(error); }
        }, "stone-storage-probe");
        worker.setDaemon(true);
        worker.start();
        worker.join(5000);
        if (worker.isAlive() || observed[0] != blocks) throw new AssertionError("filesystem space on managed thread");
        System.out.println("PASS real image Os.statvfs on main and managed threads.");
    }

    public static int run() throws Exception {
        if (twice(21) != 42) throw new AssertionError("JNI callback");
        if (fieldValue(new NullReceiver()) != 42 || virtualValue(new Object()) == null)
            throw new AssertionError("receiver access");
        try { fieldValue(null); throw new AssertionError("implicit field null check"); }
        catch (NullPointerException expected) { }
        try { virtualValue(null); throw new AssertionError("implicit virtual null check"); }
        catch (NullPointerException expected) { }
        System.out.println("PASS interpreted field and virtual null checks with exception recovery.");
        HashMap<String, Integer> values = new HashMap<String, Integer>();
        for (int i = 0; i < 100; ++i) values.put("key-" + i, i);
        if (values.get("key-42") != 42) throw new AssertionError("managed objects");
        CRC32 crc = new CRC32();
        crc.update("123456789".getBytes("UTF-8"));
        if (crc.getValue() != 0xcbf43926L) throw new AssertionError("native zlib");
        byte[] digest = MessageDigest.getInstance("SHA-256").digest("abc".getBytes("UTF-8"));
        StringBuilder encoded = new StringBuilder();
        for (byte value : digest) {
            encoded.append("0123456789abcdef".charAt((value & 255) >>> 4));
            encoded.append("0123456789abcdef".charAt(value & 15));
        }
        if (!"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad".equals(encoded.toString()))
            throw new AssertionError("image security provider SHA-256");
        if (!"/data".equals(System.getProperty("user.dir"))) throw new AssertionError("guest working directory");
        File file = new File("stone-art-probe.data");
        try {
            try (FileOutputStream output = new FileOutputStream(file)) { output.write(new byte[]{12, 14, 16}); }
            try (FileInputStream input = new FileInputStream(file)) {
                if (input.read() + input.read() + input.read() != 42 || input.read() != -1)
                    throw new AssertionError("guest file I/O");
            }
            if (!file.getCanonicalPath().startsWith("/data/")) throw new AssertionError("guest canonical path");
            verifyStorage(file.getCanonicalPath());
            if (!new File("/system/build.prop").canRead() || new File("/system/build.prop").canWrite())
                throw new AssertionError("read-only system resources");
        } finally {
            if (file.exists() && !file.delete()) throw new AssertionError("guest file cleanup");
        }
        final AtomicInteger result = new AtomicInteger();
        Thread worker = new Thread(new Runnable() {
            @Override public void run() { result.set(twice(21)); }
        }, "stone-art-probe");
        worker.setDaemon(true);
        worker.start();
        worker.join(5000);
        if (worker.isAlive() || result.get() != 42) throw new AssertionError("managed thread and JNI");
        int checksum = 0;
        for (int i = 0; i < 128; ++i) {
            byte[] block = new byte[1024 * 1024];
            block[block.length - 1] = (byte) i;
            checksum += block[block.length - 1];
        }
        if (checksum != 8128) throw new AssertionError("managed allocation");
        System.gc();
        try {
            throw new IllegalArgumentException("probe");
        } catch (IllegalArgumentException expected) {
            if (!"probe".equals(expected.getMessage())) throw new AssertionError("exception handling");
        }
        if (Boolean.getBoolean("stone.probe.jit")) verifyJit();
        return result.get();
    }
}
