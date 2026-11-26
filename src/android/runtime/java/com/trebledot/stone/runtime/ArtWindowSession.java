package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.Context;
import android.content.ContextWrapper;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;

/** Hosts in-process dialogs and popups in the same software frame as their Activity. */
final class ArtWindowSession {
    // WindowManagerGlobal.ADD_FLAG_IN_TOUCH_MODE and ADD_FLAG_APP_VISIBLE.
    private static final int VISIBLE_IN_TOUCH_MODE = 3;
    private static final int ADD_PERMISSION_DENIED = -8;
    private final Map<IBinder, Child> children = new LinkedHashMap<>();
    private final Map<Activity, ArtViewRoot> activityRoots = new LinkedHashMap<>();
    private final Map<IBinder, WindowIdentity> windowIds = new LinkedHashMap<>();
    private Activity activeActivity;
    private final Handler main = new Handler(Looper.getMainLooper());
    private Child touchTarget;
    private View activityRoot;
    private View focusedRoot;
    private boolean hostFocused;
    private int width, height;
    private Object windowManager;
    private java.util.function.Consumer<java.util.List<View>> windowViewsListener;

    void detachRemovedWindows() throws ReflectiveOperationException {
        for (Child child : new ArrayList<>(children.values()))
            if (child.removed()) child.detach();
        updateFocus();
    }

    private void observeWindowRemoval() throws ReflectiveOperationException {
        if (windowViewsListener != null) return;
        Class<?> type = Class.forName("android.view.WindowManagerGlobal");
        Method add;
        try { add = type.getDeclaredMethod("addWindowViewsListener",java.util.concurrent.Executor.class,java.util.function.Consumer.class); }
        catch (NoSuchMethodException absent) { return; } // Older images retain frame-boundary cleanup.
        windowManager = ArtViewRoot.invoke(type,null,"getInstance",new Class<?>[0]);
        windowViewsListener = views -> {
            // With compositor traversals disabled, ViewRoot.doDie skips its
            // detach. Do it synchronously when WindowManager drops that root,
            // before Compose can notify observers belonging to a removed popup.
            try { detachRemovedWindows(); }
            catch (ReflectiveOperationException error) {
                throw new IllegalStateException("Could not detach a removed ART window",error);
            }
        };
        add.setAccessible(true);
        add.invoke(windowManager,(java.util.concurrent.Executor) Runnable::run,windowViewsListener);
    }

    Object call(Object instance, Method method, Object[] args) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "addToDisplayAsUser":
                return add(args);
            case "remove": {
                Child child = children.remove(args[0]);
                if (child != null) child.closeInputChannel();
                WindowIdentity identity = windowIds.remove(args[0]);
                if (identity != null) identity.focus(false);
                return null;
            }
            case "getWindowId": return windowId((IBinder)args[0]);
            // Host input has no Android input dispatcher or compositor.
            case "setOnBackInvokedCallbackInfo":
            case "updateRequestedVisibleTypes":
            case "setInsets":
            case "onRectangleOnScreenRequested":
            // Android edge gestures are not provided by this Win32 host.
            case "reportSystemGestureExclusionChanged":
                return null;
            default: throw new UnsupportedOperationException("ART window session: " + method.getName());
        }
    }

    private Object windowId(IBinder token) throws ReflectiveOperationException {
        if (token == null) return null;
        WindowIdentity identity = windowIds.get(token);
        if (identity != null) return identity.service;
        boolean known = children.containsKey(token);
        for (ArtViewRoot root : activityRoots.values()) known |= token.equals(root.windowToken());
        if (!known) return null;
        identity = new WindowIdentity(token);
        identity.focused = focusedRoot != null && token.equals(focusedRoot.getWindowToken());
        windowIds.put(token, identity);
        return identity.service;
    }

    private static final class WindowIdentity {
        final IBinder token;
        final IInterface service;
        final Map<IBinder, Object> observers = new LinkedHashMap<>();
        volatile boolean focused;
        WindowIdentity(IBinder token) throws ReflectiveOperationException {
            this.token = token;
            String descriptor = "android.view.IWindowId";
            service = ArtServices.service(descriptor, (instance, method, args) -> {
                switch (method.getName()) {
                    case "isFocused": return focused;
                    case "registerFocusObserver":
                        synchronized(observers) { observers.put(((IInterface)args[0]).asBinder(),args[0]); } return null;
                    case "unregisterFocusObserver":
                        synchronized(observers) { observers.remove(((IInterface)args[0]).asBinder()); } return null;
                    default: throw new UnsupportedOperationException("ART window identity: " + method.getName());
                }
            }).queryLocalInterface(descriptor);
        }
        void focus(boolean value) {
            if (focused == value) return;
            focused = value;
            ArrayList<Object> snapshot;
            synchronized(observers) { snapshot=new ArrayList<>(observers.values()); }
            for (Object observer : snapshot) try {
                Class.forName("android.view.IWindowFocusObserver").getMethod(value ? "focusGained" : "focusLost", IBinder.class)
                    .invoke(observer, service.asBinder());
            } catch (ReflectiveOperationException error) {
                android.util.Log.w("StoneWindow", "Could not notify window focus observer",error);
            }
        }
    }

    private int add(Object[] args) throws ReflectiveOperationException {
        WindowManager.LayoutParams attributes = (WindowManager.LayoutParams) args[1];
        boolean toast = attributes.type == WindowManager.LayoutParams.TYPE_TOAST && ArtServices.notifications != null
            && ArtServices.notifications.toasts.allows(attributes);
        if (!toast && (attributes.type < WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW
                || attributes.type > WindowManager.LayoutParams.LAST_SUB_WINDOW)) return ADD_PERMISSION_DENIED;
        observeWindowRemoval();
        IInterface window = (IInterface) args[0];
        Object root = ((WeakReference<?>) ArtViewRoot.field(window, "mViewAncestor").get(window)).get();
        if (root == null) throw new IllegalStateException("Missing root for ART dialog or popup");
        View view = (View) ArtViewRoot.field(root, "mView").get(root);
        int w = width > 0 ? width : view.getResources().getDisplayMetrics().widthPixels;
        int h = height > 0 ? height : view.getResources().getDisplayMetrics().heightPixels;
        Rect display = new Rect(0, 0, w, h);
        Object insets;
        if (args.length == 8) {
            Object result = args[7];
            Object frames = ArtViewRoot.field(result, "frames").get(result);
            ((Rect) ArtViewRoot.field(frames, "frame").get(frames)).set(display);
            ((Rect) ArtViewRoot.field(frames, "displayFrame").get(frames)).set(display);
            insets = ArtViewRoot.field(result, "insetsState").get(result);
        } else {
            insets = args[7];
            ((Rect) args[9]).set(0, 0, -1, -1);
        }
        ArtViewRoot.invoke(insets.getClass(), insets, "setDisplayFrame", new Class<?>[]{Rect.class}, display);
        Child child = new Child(root, view, args[6]);
        // The framework owns the ViewRoot and its input receiver. Stone supplies the
        // surface-free traversal and draws the view into the Activity's frame.
        IBinder token = window.asBinder();
        child.suppressTraversal();
        children.put(token, child);
        // Some image versions mark the root added after the session returns.
        Runnable suppress = () -> {
            if (children.get(token) != child) return;
            try { child.suppressTraversal(); }
            catch (ReflectiveOperationException error) {
                throw new IllegalStateException("Could not disable compositor traversal for an ART child window", error);
            }
        };
        main.postAtFrontOfQueue(suppress);
        // Compose can show and resize a dialog inside a frame callback. The
        // same frame's traversal then runs before the main queue gets control.
        // Schedule ahead of native traversal callbacks already due in that frame.
        Object choreographer = ArtViewRoot.field(root, "mChoreographer").get(root);
        ArtViewRoot.invoke(choreographer.getClass(), choreographer, "postCallbackDelayed",
            new Class<?>[]{int.class, Runnable.class, Object.class, long.class},
            3, suppress, child, -android.os.SystemClock.uptimeMillis()); // CALLBACK_TRAVERSAL, due time zero.
        return VISIBLE_IN_TOUCH_MODE;
    }

    void draw(Canvas canvas, int width, int height, ArtSoftwareRipples ripples) throws ReflectiveOperationException {
        this.width = width;
        this.height = height;
        if (!children.isEmpty()) {
            for (Child child : new ArrayList<>(children.values())) {
                if (child.removed()) {
                    child.detach();
                    continue;
                }
                if (!isActive(child) || child.view.getVisibility() != View.VISIBLE) continue;
                child.draw(canvas, width, height, ripples);
            }
        }
        updateFocus();
    }

    boolean dispatchTouch(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) touchTarget = target(event);
        if (touchTarget == null) return false;
        Child target = touchTarget;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) touchTarget = null;
        if (!children.containsValue(target) || !isActive(target) || !target.view.isAttachedToWindow()) return true;
        event.offsetLocation(-target.bounds.left, -target.bounds.top);
        try { target.view.dispatchTouchEvent(event); }
        finally { event.offsetLocation(target.bounds.left, target.bounds.top); }
        return true;
    }

    private Child target(MotionEvent event) {
        if (children.isEmpty()) return null;
        ArrayList<Child> ordered = new ArrayList<>(children.values());
        for (int i = ordered.size() - 1; i >= 0; i--) {
            Child child = ordered.get(i);
            if (!isActive(child) || child.bounds.isEmpty() || !child.view.isShown() || !child.view.isAttachedToWindow()) continue;
            int flags = child.attributes().flags;
            if ((flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0) continue;
            boolean modal = (flags & (WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)) == 0;
            if (modal || child.bounds.contains((int) event.getX(), (int) event.getY())) return child;
        }
        return null;
    }

    boolean dispatchScroll(MotionEvent event) {
        Child child = target(event);
        if (child == null) return false;
        event.offsetLocation(-child.bounds.left, -child.bounds.top);
        try { child.view.dispatchGenericMotionEvent(event); }
        finally { event.offsetLocation(child.bounds.left, child.bounds.top); }
        return true;
    }

    View keyTarget() {
        View focused = null;
        for (Child child : children.values()) {
            if (isActive(child) && child.view.isShown() && child.view.isAttachedToWindow()
                    && (child.attributes().flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0)
                focused = child.view;
        }
        return focused;
    }

    void focusChanged(View activityRoot, boolean focused) {
        hostFocused = focused;
        setActivityRoot(activityRoot);
    }

    void setActivityRoot(View activityRoot) {
        this.activityRoot = activityRoot;
        updateFocus();
    }

    void registerActivityRoot(Activity activity, ArtViewRoot root) {
        if (activityRoots.putIfAbsent(activity, root) != null)
            throw new IllegalStateException("Activity already has a software root");
    }

    void unregisterActivityRoot(Activity activity, ArtViewRoot root) {
        activityRoots.remove(activity, root);
        try { windowIds.remove(root.windowToken()); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not release ART window identity",error); }
        if (activeActivity == activity) activeActivity = null;
    }

    ArtViewRoot showActivity(Activity activity) throws ReflectiveOperationException {
        if (activeActivity != activity) { clearFocus(); touchTarget = null; }
        ArtViewRoot root = activityRoots.get(activity);
        if (root == null) root = ArtViewRoot.attach(activity);
        activeActivity = activity;
        for (Map.Entry<Activity, ArtViewRoot> entry : new ArrayList<>(activityRoots.entrySet()))
            entry.getValue().setVisible(entry.getKey() == activity);
        for (Child child : new ArrayList<>(children.values()))
            if (child.software != null) child.software.setVisible(isActive(child));
        setActivityRoot(activity.getWindow().getDecorView());
        return root;
    }

    boolean captureSecure(Activity fallback) {
        Activity current = activeActivity != null ? activeActivity : ArtServices.activities.currentActivity(fallback);
        if (current != null && (current.getWindow().getAttributes().flags & WindowManager.LayoutParams.FLAG_SECURE) != 0) return true;
        // All visible Android windows share one Windows compositor surface.
        // A secure child therefore protects that entire host surface.
        for (Child child : children.values()) {
            if (isActive(child) && child.view.getVisibility() == View.VISIBLE && child.view.getParent() != null
                    && (child.attributes().flags & WindowManager.LayoutParams.FLAG_SECURE) != 0) return true;
        }
        return false;
    }

    private boolean isActive(Child child) {
        Activity current = activeActivity != null ? activeActivity : ArtServices.activities.currentActivity(null);
        Activity owner = child.owner();
        return owner == null || owner == current;
    }

    void clearActivityFocus(Activity activity) {
        if (activity != null && (activeActivity == activity || activityRoot == activity.getWindow().getDecorView()))
            clearFocus();
    }

    void stopActivitySurfaceProducers(Activity activity) {
        ArtViewRoot root=activityRoots.get(activity);
        if(root!=null) root.stopSurfaceProducers();
        for(Child child:children.values()) if(child.owner()==activity && child.software!=null) child.software.stopSurfaceProducers();
    }
    void closeActivity(Activity activity) throws ReflectiveOperationException {
        clearActivityFocus(activity);
        if (touchTarget != null && touchTarget.owner() == activity) touchTarget = null;
        ArtViewRoot root = activityRoots.get(activity);
        try { if (root != null) root.close(); }
        finally { closeActivityWindows(activity); }
    }

    private void updateFocus() {
        View next = hostFocused ? keyTarget() : null;
        if (hostFocused && next == null) next = activityRoot;
        if (next == focusedRoot) return;
        View previous=focusedRoot;
        focusedRoot = next;
        for (WindowIdentity identity : new ArrayList<>(windowIds.values()))
            identity.focus(focusedRoot != null && identity.token.equals(focusedRoot.getWindowToken()));
        if (previous != null) ArtViewRoot.setWindowFocus(previous, false);
        if (focusedRoot != null) ArtViewRoot.setWindowFocus(focusedRoot, true);
    }

    void clearFocus() {
        View previous = focusedRoot;
        focusedRoot = null;
        activityRoot = null;
        for (WindowIdentity identity : new ArrayList<>(windowIds.values())) identity.focus(false);
        if (previous != null) ArtViewRoot.setWindowFocus(previous, false);
    }

    boolean dispatchBack() {
        View focused = keyTarget();
        if (focused == null) return false;
        if (ArtViewRoot.dispatchBackCallback(focused)) return true;
        long now = android.os.SystemClock.uptimeMillis();
        focused.dispatchKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0));
        focused.dispatchKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0));
        return true;
    }

    void close() throws ReflectiveOperationException {
        if (ArtServices.notifications != null) ArtServices.notifications.toasts.close();
        touchTarget = null;
        focusChanged(null, false);
        Throwable failure = null;
        for (Activity activity : new ArrayList<>(activityRoots.keySet())) {
            try { closeActivity(activity); }
            catch (ReflectiveOperationException | RuntimeException | Error error) {
                if (failure == null) failure = error; else failure.addSuppressed(error);
            }
        }
        try { closeActivityWindows(null); }
        catch (ReflectiveOperationException | RuntimeException | Error error) {
            if (failure == null) failure = error; else failure.addSuppressed(error);
        }
        activeActivity = null;
        windowIds.clear();
        if (windowViewsListener != null) {
            ArtViewRoot.invoke(windowManager.getClass(),windowManager,"removeWindowViewsListener",
                new Class<?>[]{java.util.function.Consumer.class},windowViewsListener);
            windowViewsListener = null; windowManager = null;
        }
        if (failure instanceof ReflectiveOperationException) throw (ReflectiveOperationException) failure;
        if (failure instanceof RuntimeException) throw (RuntimeException) failure;
        if (failure instanceof Error) throw (Error) failure;
    }

    void closeActivityWindows(Activity activity) throws ReflectiveOperationException {
        for (Child child : new ArrayList<>(children.values())) {
            if (activity != null && child.owner() != activity) continue;
            if (!child.removed()) {
                WindowManager manager = (WindowManager) child.view.getContext().getSystemService(Context.WINDOW_SERVICE);
                manager.removeViewImmediate(child.view);
            }
            child.detach();
        }
    }

    private static final class Child {
        final Object root;
        final View view;
        final Rect bounds = new Rect();
        Object inputPeer;
        ArtViewRoot software;
        boolean detached;

        Child(Object root, View view, Object input) throws ReflectiveOperationException {
            this.root = root;
            this.view = view;
            if (input == null) return;
            Class<?> type = input.getClass();
            Object[] pair = (Object[]) ArtViewRoot.invoke(type, null, "openInputChannelPair",
                new Class<?>[]{String.class}, "Stone " + view.getClass().getSimpleName());
            try { ArtViewRoot.invoke(type, pair[1], "copyTo", new Class<?>[]{type}, input); }
            finally { ArtViewRoot.invoke(type, pair[1], "dispose", new Class<?>[0]); }
            inputPeer = pair[0];
        }

        void closeInputChannel() throws ReflectiveOperationException {
            if (inputPeer == null) return;
            ArtViewRoot.invoke(inputPeer.getClass(), inputPeer, "dispose", new Class<?>[0]);
            inputPeer = null;
        }

        boolean removed() throws ReflectiveOperationException {
            return ArtViewRoot.field(root, "mRemoved").getBoolean(root);
        }

        void suppressTraversal() throws ReflectiveOperationException {
            ArtViewRoot.field(root, "mAdded").setBoolean(root, false);
            ArtViewRoot.invoke(root.getClass(), root, "unscheduleTraversals", new Class<?>[0]);
        }

        Activity owner() {
            Context context = view.getContext();
            while (context instanceof ContextWrapper) {
                if (context instanceof Activity) return (Activity) context;
                Context base = ((ContextWrapper) context).getBaseContext();
                if (base == context) break;
                context = base;
            }
            return null;
        }

        void detach() throws ReflectiveOperationException {
            if (detached) return;
            detached = true;
            ArtViewRoot.invoke(root.getClass(), root, "dispatchDetachedFromWindow", new Class<?>[0]);
        }

        WindowManager.LayoutParams attributes() {
            return (WindowManager.LayoutParams) view.getLayoutParams();
        }

        void draw(Canvas canvas, int width, int height, ArtSoftwareRipples ripples) throws ReflectiveOperationException {
            if (software == null) software = ArtViewRoot.attachChild(view, root);
            software.setVisible(true);
            WindowManager.LayoutParams attrs = attributes();
            boolean layout = view.isLayoutRequested() || bounds.isEmpty();
            view.measure(spec(attrs.width, width), spec(attrs.height, height));
            int w = view.getMeasuredWidth(), h = view.getMeasuredHeight();
            int x = attrs.x + Math.round(attrs.horizontalMargin * width);
            int y = attrs.y + Math.round(attrs.verticalMargin * height);
            Gravity.apply(attrs.gravity, w, h, new Rect(0, 0, width, height), x, y, bounds, view.getLayoutDirection());
            software.prepareFrame(bounds.left, bounds.top, w, h, width, height);
            view.layout(0, 0, w, h);
            android.view.ViewTreeObserver observer = view.getViewTreeObserver();
            if (layout) observer.dispatchOnGlobalLayout();
            if (observer.dispatchOnPreDraw()) return;
            if ((attrs.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0)
                canvas.drawColor(Color.argb(Math.round(255 * Math.max(0f, Math.min(1f, attrs.dimAmount))), 0, 0, 0));
            int save = canvas.save();
            try {
                canvas.clipRect(bounds);
                canvas.translate(bounds.left, bounds.top);
                observer.dispatchOnDraw();
                if (ripples != null) ripples.prepare(view);
                view.draw(canvas);
            } finally { canvas.restoreToCount(save); }
        }

        private int spec(int size, int available) {
            if (size == WindowManager.LayoutParams.WRAP_CONTENT)
                return View.MeasureSpec.makeMeasureSpec(available, View.MeasureSpec.AT_MOST);
            return View.MeasureSpec.makeMeasureSpec(size < 0 ? available : Math.min(size, available), View.MeasureSpec.EXACTLY);
        }
    }
}
