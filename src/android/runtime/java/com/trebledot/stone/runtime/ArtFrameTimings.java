package com.trebledot.stone.runtime;

import android.app.Activity;
import android.util.Log;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;

/** Opt-in frame timings without screenshots, stack suspension or per-frame I/O. */
final class ArtFrameTimings implements AutoCloseable {
    private static final boolean ENABLED = "1".equals(System.getenv("STONE_ART_FRAME_TIMINGS"));
    static final int PREPARE = 6, LAYOUT = 7, PAINT = 8, CHILDREN = 9, PIXELS = 10, PRESENT = 11, DIAGNOSTICS = 12;
    private static final int TOTAL = 13, COLUMNS = 14, CAPACITY = 8192;
    private final long[] samples = new long[CAPACITY * COLUMNS];
    private final File directory;
    private int count, dropped, row;
    private long epoch, started, marked, previousEnd;
    private boolean closed;

    static ArtFrameTimings create(Activity activity) { return ENABLED ? new ArtFrameTimings(activity) : null; }
    private ArtFrameTimings(Activity activity) {
        directory = new File("/data/stone-diagnostics", activity.getPackageName());
    }
    void begin() {
        long now = System.nanoTime();
        if (epoch == 0) epoch = now;
        started = marked = now;
        row = count < CAPACITY ? count * COLUMNS : -1;
        if (row >= 0) {
            samples[row] = count;
            samples[row + 1] = now - epoch;
            samples[row + 2] = previousEnd == 0 ? 0 : now - previousEnd;
        }
    }
    void mark(int column) {
        long now = System.nanoTime();
        if (row >= 0) samples[row + column] = now - marked;
        marked = now;
    }
    void repainted(boolean value) { if (row >= 0) samples[row + 4] = value ? 1 : 0; }
    void cancelled() { if (row >= 0) samples[row + 5] = 1; }
    void end(int touches) {
        long now = System.nanoTime();
        if (row >= 0) {
            samples[row + 3] = touches;
            samples[row + TOTAL] = now - started;
            count++;
        } else dropped++;
        previousEnd = now;
    }
    @Override public void close() {
        if (closed) return;
        closed = true;
        if (count == 0) return;
        try {
            if (!directory.isDirectory() && !directory.mkdirs()) throw new java.io.IOException("Cannot create timing directory");
            try (Writer output = new BufferedWriter(new OutputStreamWriter(
                    new FileOutputStream(new File(directory,"frame-timings.csv")), StandardCharsets.UTF_8))) {
                output.write("# dropped=" + dropped + "\n");
                output.write("frame,start_ns,gap_ns,touches,repainted,cancelled,prepare_ns,layout_ns,paint_ns,children_ns,pixels_ns,present_ns,diagnostics_ns,total_ns\n");
                for (int index = 0; index < count; ++index) {
                    for (int column = 0; column < COLUMNS; ++column) {
                        if (column != 0) output.write(',');
                        output.write(Long.toString(samples[index * COLUMNS + column]));
                    }
                    output.write('\n');
                }
            }
        } catch (java.io.IOException | RuntimeException error) { Log.w("StoneFrameTimings","Cannot write frame timings",error); }
    }
}
