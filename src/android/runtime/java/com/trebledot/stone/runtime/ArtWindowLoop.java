package com.trebledot.stone.runtime;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Handler;
import android.os.Looper;
import android.view.View;

/** Main-thread software rendering while the image's ViewRoot has no Android compositor. */
final class ArtWindowLoop implements AutoCloseable {
    // Diagnostic control: compare the same binary with full repainting.
    private static final boolean REDRAW_EVERY_FRAME = Boolean.getBoolean("stone.diagnostics")
        && "1".equals(System.getenv("STONE_ART_REDRAW_EVERY_FRAME"));
    private static final class WindowClosed extends Error {
        WindowClosed() { super("Stone window closed", null, false, false); }
    }

    private Activity activity;
    private ArtViewRoot root;
    private Bitmap bitmap;
    private final Canvas canvas;
    private final ArtSoftwareRipples ripples;
    private int[] pixels;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final WindowClosed stopped = new WindowClosed();
    private final Runnable frame = this::dispatchFrame;
    private Runnable windowClosed;
    private boolean closed;
    private Activity orientationActivity;
    private int orientationRequest = Integer.MIN_VALUE;
    private Boolean userLandscape;
    private final ArtDiagnostics diagnostics;
    private final ArtFrameTimings timings;

    ArtWindowLoop(Activity activity, int width, int height) throws ReflectiveOperationException {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || (long) width * height > 64 * 1024 * 1024)
            throw new IllegalArgumentException("Invalid ART software window dimensions");
        this.activity = activity;
        timings = ArtFrameTimings.create(activity);
        diagnostics = new ArtDiagnostics(activity);
        ripples = ArtSoftwareRipples.forImage();
        // Allocate before attaching: allocation failures must leave the decor untouched.
        pixels = new int[width * height];
        bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        canvas = new Canvas(bitmap);
        try { root = ArtViewRoot.attach(activity); }
        catch (ReflectiveOperationException | RuntimeException | Error error) {
            canvas.setBitmap(null);
            bitmap.recycle();
            throw error;
        }
    }

    void run() {
        try {
            start(null);
            Looper.loop();
        }
        catch (WindowClosed signal) { if (signal != stopped) throw signal; }
        finally { diagnostics.close(); if (timings != null) timings.close(); handler.removeCallbacks(frame); }
    }

    void start(Runnable onClosed) {
        windowClosed = onClosed;
        try { root = ArtServices.windows.showActivity(activity); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot show ART Activity", error); }
        diagnostics.start(); ArtWindow.open(activity);
        dispatchFrame();
    }

    private void dispatchFrame() {
        if (closed) return;
        try { nextFrame(); }
        catch (WindowClosed signal) {
            if (signal != stopped || windowClosed == null) throw signal;
            try { close(); }
            catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot close ART window", error); }
            windowClosed.run();
        }
    }

    private void nextFrame() {
        if (!ArtWindow.isOpen()) throw stopped;
        if (timings != null) timings.begin();
        ArtConfiguration.poll();
        updateOrientation();
        updateSize();
        if (!updateActivity()) {
            // A result callback may finish its own caller before that queued
            // finish is processed. Its predecessor's decor is already detached.
            // Leave input queued until the task has a live, attached target.
            if (timings != null) { timings.cancelled(); timings.end(0); }
            if (!ArtWindow.isOpen()) throw stopped;
            handler.postDelayed(frame, 16);
            return;
        }
        int touches = ArtWindow.pollTouches(activity);
        if (timings != null) timings.mark(ArtFrameTimings.PREPARE);
        if (!ArtWindow.isOpen()) throw stopped;
        render();
        if (timings != null) timings.end(touches);
        handler.postDelayed(frame, 16);
    }

    private boolean updateActivity() {
        Activity current = ArtServices.activities.currentActivity(activity);
        if (current == null || current.isFinishing() || current.isDestroyed()) return false;
        if (current == activity && current.getWindow().getDecorView().isAttachedToWindow()) return true;
        try {
            root = ArtServices.windows.showActivity(current);
            activity = current;
            return true;
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Could not switch the ART window to the next Activity", error);
        }
    }

    private void updateSize() {
        long size = ArtWindow.clientSize();
        int width = (int) (size >>> 32), height = (int) size;
        if (width <= 0 || height <= 0) return;
        if (width == bitmap.getWidth() && height == bitmap.getHeight()) { ArtConfiguration.resize(width, height); return; }
        if (width > 16384 || height > 16384 || (long) width * height > 64 * 1024 * 1024)
            throw new IllegalStateException("ART software window exceeds maximum frame size");
        int[] nextPixels = new int[width * height];
        Bitmap next = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        canvas.setBitmap(next);
        bitmap.recycle();
        bitmap = next; pixels = nextPixels;
        ArtConfiguration.resize(width, height);
    }

    private void updateOrientation() {
        Activity current = ArtServices.activities.currentActivity(activity);
        if(current==null || current.isDestroyed() || current.isFinishing()) return;
        int request = current.getRequestedOrientation();
        if(current==orientationActivity && request==orientationRequest) return;
        orientationActivity=current; orientationRequest=request;
        Boolean wide=null;
        switch(request) {
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE: wide=true; break;
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT: wide=false; break;
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LOCKED:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_NOSENSOR: return;
        }
        if(wide!=null) {
            long size=ArtWindow.clientSize();
            if(size==0) return;
            if(userLandscape==null) userLandscape=(int)(size>>>32)>(int)size;
            ArtWindow.orient(wide);
        } else if(userLandscape!=null) {
            ArtWindow.orient(userLandscape); userLandscape=null;
        }
    }

    private void render() {
        View decor = activity.getWindow().getDecorView();
        canvas.setDensity(activity.getResources().getDisplayMetrics().densityDpi);
        int width = bitmap.getWidth(), height = bitmap.getHeight();
        root.prepareFrame(width, height);
        boolean layout = decor.isLayoutRequested() || decor.getWidth() != width || decor.getHeight() != height;
        decor.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        decor.layout(0, 0, width, height);
        try { ArtSurfaceViews.suppressCompositor(decor); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not prepare software SurfaceViews",error); }
        try { root.updateSurfaces(); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not update software SurfaceViews",error); }
        android.view.ViewTreeObserver observer = decor.getViewTreeObserver();
        if (layout) observer.dispatchOnGlobalLayout();
        boolean cancelled = observer.dispatchOnPreDraw();
        if (timings != null) timings.mark(ArtFrameTimings.LAYOUT);
        if (cancelled) { if (timings != null) timings.cancelled(); return; }
        if (!ArtWindow.isOpen()) throw stopped;
        bitmap.eraseColor(0);
        try {
            boolean repainted = root.draw(canvas, ripples, layout || REDRAW_EVERY_FRAME);
            if (timings != null) timings.repainted(repainted);
        }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not draw ART Activity", error); }
        if (timings != null) timings.mark(ArtFrameTimings.PAINT);
        try { ArtServices.windows.draw(canvas, width, height, ripples); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not draw ART child windows", error); }
        if (timings != null) timings.mark(ArtFrameTimings.CHILDREN);
        if (!ArtWindow.isOpen()) throw stopped;
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        if (timings != null) timings.mark(ArtFrameTimings.PIXELS);
        boolean presented = ArtWindow.present(pixels, width, height);
        if (timings != null) timings.mark(ArtFrameTimings.PRESENT);
        if (!ArtWindow.isOpen()) throw stopped;
        if (!presented) throw new IllegalStateException("Could not present ART software window");
        diagnostics.capture(activity, bitmap);
        if (timings != null) timings.mark(ArtFrameTimings.DIAGNOSTICS);
    }

    @Override public void close() throws ReflectiveOperationException {
        if (closed) return;
        closed = true;
        diagnostics.close();
        if (timings != null) timings.close();
        handler.removeCallbacks(frame);
        try {
            ArtServices.windows.clearFocus();
            try { root.close(); }
            finally { ArtServices.windows.close(); }
        }
        finally {
            canvas.setBitmap(null);
            bitmap.recycle();
            ArtWindow.close();
        }
    }
}
