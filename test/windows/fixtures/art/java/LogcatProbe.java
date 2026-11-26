package com.trebledot.stone.probe;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Exercises image libcore's real Process streams against the Service journal. */
final class LogcatProbe {
    static String read(InputStream stream) throws Exception {
        try (InputStream input = stream) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream(); byte[] buffer = new byte[2048]; int count;
            while ((count = input.read(buffer)) != -1) {
                if (bytes.size() + count > 2100000) throw new AssertionError("Unbounded application log snapshot");
                bytes.write(buffer,0,count);
            }
            return new String(bytes.toByteArray(),StandardCharsets.UTF_8);
        }
    }
    static void verify(String marker, String previous) throws Exception {
        String command = "logcat -d --uid=" + android.os.Process.myUid() + " *:V";
        android.util.Log.i("StoneLogcatProbe",marker + " 中文日志 😀");
        for (int i = 0; i < 3; ++i) {
            Process process = Runtime.getRuntime().exec(command);
            // Wait before reading on alternate runs to exercise libcore's reaper drain.
            if (i != 1 && process.waitFor() != 0) throw new AssertionError("Logcat command failed");
            String text = read(process.getInputStream());
            if (process.waitFor() != 0 || !read(process.getErrorStream()).isEmpty()) throw new AssertionError("Logcat exit/error stream differs");
            if (!text.contains("StoneLogcatProbe: " + marker + " 中文日志 😀")) throw new AssertionError("Logcat omitted the real Java log record: " + text);
            if (!previous.isEmpty() && !text.contains(previous)) throw new AssertionError("Logcat lost the previous Client's committed record");
        }
        Process destroyed = Runtime.getRuntime().exec(command); destroyed.destroy();
        if (destroyed.waitFor() != 0) throw new AssertionError("Completed logcat result changed after destroy");
        try { destroyed.getInputStream().read(); throw new AssertionError("Destroy did not close the Process input stream"); }
        catch (java.io.IOException expected) { }
        for (String[] invalid : new String[][]{
                {"logcat","-d","--uid=" + (android.os.Process.myUid()+1),"*:V"},
                {"logcat","-d","*:V"}, {"logcat"}, {"/system/bin/sh","-c",command}, {"stone-unsupported-executable"}}) {
            try { Process process = Runtime.getRuntime().exec(invalid); process.destroy(); throw new AssertionError("Unsupported process/UID was accepted"); }
            catch (java.io.IOException expected) { }
        }
        System.out.println("PASS real logcat streams, EOF, wait-before-read, destroy, current UID and unsupported command rejection; marker=" + marker);
    }
}
