package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.view.Display;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewParent;
import android.window.OnBackInvokedCallback;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.InvocationTargetException;

/** Connects the image's view root to the Windows software frame host. */
final class ArtViewRoot implements AutoCloseable {
    private final Activity activity;
    private final Object root;
    private final Object info;
    private final View view;
    private boolean attached;
    private android.graphics.Bitmap layer;
    private android.graphics.Canvas layerCanvas;
    private android.graphics.Rect dirty;
    private Field animating, ignoreDirtyState;
    private boolean layerValid;
    private final ArtSurfaceViews surfaces = new ArtSurfaceViews();

    private ArtViewRoot(Activity activity) throws ReflectiveOperationException {
        this.activity = activity;
        view = activity.getWindow().getDecorView();
        if (view.isAttachedToWindow() || view.getParent() != null)
            throw new IllegalStateException("Activity decor is already attached");
        root = create(activity);
        info = field(root, "mAttachInfo").get(root);
        field(root, "mView").set(root, view);
        field(info, "mRootView").set(info, view);
        field(info, "mWindowVisibility").setInt(info, View.VISIBLE);
        // Windows owns software frame layout and presentation. Keep mAdded false:
        // ViewRoot's SurfaceFlinger traversal must not allocate Android surfaces.
        invoke(View.class, view, "assignParent", new Class<?>[]{ViewParent.class}, root);
        attached = true;
        try {
            ArtServices.windows.registerActivityRoot(activity, this);
            invoke(View.class, view, "dispatchAttachedToWindow", new Class<?>[]{info.getClass(), int.class}, info, View.VISIBLE);
            ArtSurfaceViews.suppressCompositor(view);
        } catch (ReflectiveOperationException | RuntimeException | Error error) {
            try { close(); } catch (Throwable cleanup) { error.addSuppressed(cleanup); }
            throw error;
        }
    }

    static ArtViewRoot attach(Activity activity) throws ReflectiveOperationException { return new ArtViewRoot(activity); }

    IBinder windowToken() throws ReflectiveOperationException {
        return ((android.os.IInterface) field(root,"mWindow").get(root)).asBinder();
    }

    private ArtViewRoot(View view, Object root) throws ReflectiveOperationException {
        activity = null;
        this.view = view;
        this.root = root;
        info = field(root, "mAttachInfo").get(root);
    }

    static ArtViewRoot attachChild(View view, Object root) throws ReflectiveOperationException {
        ArtViewRoot window = new ArtViewRoot(view, root);
        field(window.info, "mWindowVisibility").setInt(window.info, View.VISIBLE);
        invoke(View.class, view, "dispatchAttachedToWindow", new Class<?>[]{window.info.getClass(), int.class},
            window.info, View.VISIBLE);
        return window;
    }

    static void dispatchBack(Activity activity) {
        if (dispatchBackCallback(activity.getWindow().getDecorView())) return;
        long now = SystemClock.uptimeMillis();
        activity.dispatchKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0));
        activity.dispatchKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0));
    }

    static void setWindowFocus(View view, boolean focused) {
        if (!view.isAttachedToWindow()) return;
        try {
            Object root = view.getParent();
            // WindowManager may already have removed a popup during this frame.
            if (root == null) return;
            Object info = field(root, "mAttachInfo").get(root);
            field(info, "mHasWindowFocus").setBoolean(info, focused);
            view.dispatchWindowFocusChanged(focused);
            // Compose observes window focus through the tree observer rather
            // than only View.onWindowFocusChanged (caret and selection UI).
            invoke(android.view.ViewTreeObserver.class, view.getViewTreeObserver(),
                "dispatchOnWindowFocusChange", new Class<?>[]{boolean.class}, focused);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Could not change ART window focus", error);
        }
    }

    static boolean dispatchBackCallback(View view) {
        if (Build.VERSION.SDK_INT < 33) return false;
        ViewParent root = view.getParent();
        try {
            Object dispatcher = invoke(root.getClass(), root, "getOnBackInvokedDispatcher", new Class<?>[0]);
            Class<?> type = dispatcher.getClass();
            if (!(boolean) invoke(type, dispatcher, "isOnBackInvokedCallbackEnabled", new Class<?>[0])) return false;
            Object callback = invoke(type, dispatcher, "getTopCallback", new Class<?>[0]);
            if (callback == null) return false;
            invoke(type, dispatcher, "onBackInvoked", new Class<?>[]{OnBackInvokedCallback.class}, callback);
            return true;
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Could not dispatch ART back navigation", error);
        }
    }

    void prepareFrame(int width, int height) {
        try {
            // Drain removed child roots before Activity drawing can publish
            // Compose snapshots to their old position/layout observers.
            ArtServices.windows.detachRemovedWindows();
            prepareFrame(0, 0, width, height, width, height);
        }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not update ART software frame", error); }
    }

    /** Reuse the Activity raster until the image reports damage or animation. */
    void updateSurfaces() throws ReflectiveOperationException { surfaces.update(view); }

    boolean draw(android.graphics.Canvas canvas, ArtSoftwareRipples ripples, boolean force)
            throws ReflectiveOperationException {
        int width = canvas.getWidth(), height = canvas.getHeight();
        if (dirty == null) {
            dirty = (android.graphics.Rect) field(root, "mDirty").get(root);
            animating = field(root, "mIsAnimating");
            try { ignoreDirtyState = field(info, "mIgnoreDirtyState"); }
            catch (NoSuchFieldException absent) { /* Newer images removed this optimization flag. */ }
        }
        if (layer == null || layer.getWidth() != width || layer.getHeight() != height || layer.getDensity() != canvas.getDensity()) {
            releaseLayer();
            layer = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888);
            layer.setDensity(canvas.getDensity());
            layerCanvas = new android.graphics.Canvas(layer);
        }
        // Like ViewRootImpl, notify draw observers before deciding whether
        // damage needs painting; deferred framework work may invalidate here.
        view.getViewTreeObserver().dispatchOnDraw();
        boolean surfaceChanged = surfaces.update(view);
        boolean redraw = force || surfaceChanged || !layerValid || view.isDirty() || !dirty.isEmpty() || animating.getBoolean(root);
        if (redraw) {
            // Consume before drawing: invalidation issued by onDraw must survive
            // for the following frame, including legacy View animations.
            dirty.setEmpty();
            animating.setBoolean(root, false);
            boolean ignored = ignoreDirtyState != null && ignoreDirtyState.getBoolean(info);
            if (ignoreDirtyState != null) ignoreDirtyState.setBoolean(info, true);
            layerValid = false;
            int save = layerCanvas.save();
            try {
                if (ripples != null) ripples.prepare(view);
                layer.eraseColor(0);
                view.draw(layerCanvas);
                layerValid = true;
            } finally {
                layerCanvas.restoreToCount(save);
                if (ignoreDirtyState != null) ignoreDirtyState.setBoolean(info, ignored);
            }
        }
        surfaces.draw(canvas);
        canvas.drawBitmap(layer, 0, 0, null);
        return redraw;
    }

    private void releaseLayer() {
        layerValid = false;
        if (layerCanvas != null) { layerCanvas.setBitmap(null); layerCanvas = null; }
        if (layer != null) { layer.recycle(); layer = null; }
    }

    void setVisible(boolean visible) throws ReflectiveOperationException {
        if (!view.isAttachedToWindow() || view.getParent() == null) return;
        int visibility = visible ? View.VISIBLE : View.GONE;
        Field field = field(info, "mWindowVisibility");
        if (field.getInt(info) == visibility) return;
        field.setInt(info, visibility);
        view.dispatchWindowVisibilityChanged(visibility);
    }

    void prepareFrame(int left, int top, int width, int height, int displayWidth, int displayHeight)
            throws ReflectiveOperationException {
        field(info, "mDrawingTime").setLong(info, android.os.SystemClock.uptimeMillis());
        field(root, "mWidth").setInt(root, width);
        field(root, "mHeight").setInt(root, height);
        ((android.graphics.Rect) field(root, "mWinFrame").get(root)).set(left, top, left + width, top + height);
        field(info, "mWindowLeft").setInt(info, left);
        field(info, "mWindowTop").setInt(info, top);
        // Surface-free roots never relayout through WindowManager. Supply the
        // frame used by View.getWindowVisibleDisplayFrame, also for small popups.
        Object frames = field(root, "mTmpFrames").get(root);
        ((android.graphics.Rect) field(frames, "displayFrame").get(frames)).set(0, 0, displayWidth, displayHeight);
    }

    void stopSurfaceProducers() { surfaces.stopProducers(); }
    @Override public void close() throws ReflectiveOperationException {
        if (android.os.Looper.myLooper() != android.os.Looper.getMainLooper())
            throw new IllegalStateException("ART view root must be detached on the main thread");
        if (!attached) return;
        ArtServices.windows.clearActivityFocus(activity);
        surfaces.close();
        try { invoke(View.class, view, "dispatchDetachedFromWindow", new Class<?>[0]); }
        finally {
            try { invoke(View.class, view, "assignParent", new Class<?>[]{ViewParent.class}, (Object) null); }
            finally {
                field(root, "mView").set(root, null);
                field(info, "mRootView").set(info, null);
                releaseLayer();
                attached = false;
                ArtServices.windows.unregisterActivityRoot(activity, this);
            }
        }
    }

    static Field field(Object target, String name) throws ReflectiveOperationException {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    static Object invoke(Class<?> type, Object target, String name, Class<?>[] parameters, Object... arguments)
            throws ReflectiveOperationException {
        Method method = type.getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        try { return method.invoke(target, arguments); }
        catch (InvocationTargetException error) { throwCause(error); return null; }
    }

    private static void throwCause(InvocationTargetException error) throws InvocationTargetException {
        if (error.getCause() instanceof Error) throw (Error) error.getCause();
        if (error.getCause() instanceof RuntimeException) throw (RuntimeException) error.getCause();
        throw error;
    }
    private static Object create(Activity activity) throws ReflectiveOperationException {
        Class<?> type = Class.forName("android.view.ViewRootImpl");
        try { return type.getConstructor(Context.class, Display.class).newInstance(activity, activity.getDisplay()); }
        catch (InvocationTargetException error) {
            throwCause(error); return null;
        }
    }
}
