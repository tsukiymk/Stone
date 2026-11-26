package com.trebledot.stone.runtime;

import android.app.Activity;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

/** Presents software ART frames through Stone's Win32 window. Calls run on the main thread. */
public final class ArtWindow {
    private static final int BACK_ACTION = -1;
    private static final int KEY_DOWN = -2, KEY_UP = -3, TEXT = -4, FOCUS = -5;
    private static final int SCROLL_VERTICAL = -6, SCROLL_HORIZONTAL = -7;
    private static final int EVENT_SIZE = 4;
    private ArtWindow() {}
    private static long touchDown;
    private static boolean touchGesture;
    private static long keyDown;
    private static char highSurrogate;
    private static boolean running;
    public static AutoCloseable attach(android.app.Activity activity) throws ReflectiveOperationException {
        mainThread();
        return ArtViewRoot.attach(activity);
    }
    public static boolean present(int[] pixels, int width, int height) {
        mainThread();
        return nativePresent(pixels, width, height, ArtServices.windows.captureSecure(null));
    }
    public static boolean close() {
        mainThread();
        highSurrogate = 0;
        return nativeClose();
    }
    /** Runs the Windows software window until it closes; Activity lifecycle remains caller-owned. */
    public static void run(android.app.Activity activity, int width, int height) throws ReflectiveOperationException {
        mainThread();
        if (running) throw new IllegalStateException("An ART window loop is already running");
        running = true;
        try (ArtWindowLoop loop = new ArtWindowLoop(activity, width, height)) { loop.run(); }
        finally { running = false; }
    }
    static boolean isOpen() { return nativeIsOpen(); }
    static long clientSize() { return nativeClientSize(); }
    static void orient(boolean wide) {
        mainThread();
        if(!nativeSetOrientation(wide)) throw new IllegalStateException("Could not apply Activity orientation");
    }
    static void minimize() { mainThread(); nativeMinimize(); }
    static void setVisible(boolean visible) { mainThread(); nativeSetVisible(visible); }
    static void open(Activity activity) {
        String packageName = activity.getPackageName();
        String prefix = "Trebledot.Stone.";
        String appId = prefix + packageName;
        if (appId.length() > 128) appId = prefix + UUID.nameUUIDFromBytes(packageName.getBytes(StandardCharsets.UTF_8));
        nativeSetAppUserModelId(appId);
        nativeOpen();
        if (!nativeSetResizable(ArtConfiguration.resizableWindow()))
            throw new IllegalStateException("Could not apply ART window resize policy");

        ArtAppearance.changed();
    }

    static boolean updateIcon(byte[] png) { return nativeSetIcon(png); }
    static void updateAppearance(String label, String path, String command) { nativeSetAppearance(label,path,command); }
    /** Dispatches queued Win32 input to the Activity and returns the number of touch events. */
    public static int pollTouches(android.app.Activity activity) {
        mainThread();
        if (!activity.getWindow().getDecorView().isAttachedToWindow())
            throw new IllegalStateException("ART input requires an attached Activity decor");
        int[] events = nativePollInput();
        if (events.length % EVENT_SIZE != 0) throw new IllegalStateException("Invalid ART window input queue");
        int touches = 0;
        for (int offset = 0; offset < events.length; offset += EVENT_SIZE) {
            int action = events[offset];
            int first = events[offset + 1], second = events[offset + 2], third = events[offset + 3];
            if (action < 0) {
                switch (action) {
                    case BACK_ACTION:
                        if (nativeIsOpen() && !ArtServices.windows.dispatchBack()) ArtViewRoot.dispatchBack(activity);
                        break;
                    case KEY_DOWN:
                    case KEY_UP:
                        long now = SystemClock.uptimeMillis();
                        if (action == KEY_DOWN && third == 0) keyDown = now;
                        dispatchKey(activity, new KeyEvent(keyDown, now,
                            action == KEY_DOWN ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP,
                            first, third, second, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD));
                        break;
                    case TEXT: {
                        // Preserve edit callbacks and input order, but avoid opening
                        // the same editor connection for every queued character.
                        try (ArtInputMethod.TextSession input = new ArtInputMethod.TextSession()) {
                            do {
                                dispatchText(activity, (char) events[offset + 1], input);
                                offset += EVENT_SIZE;
                            } while (offset < events.length && events[offset] == TEXT);
                            offset -= EVENT_SIZE;
                        }
                        break;
                    }
                    case FOCUS:
                        ArtServices.windows.focusChanged(activity.getWindow().getDecorView(), first != 0);
                        if (first == 0) highSurrogate = 0;
                        break;
                    case SCROLL_VERTICAL:
                    case SCROLL_HORIZONTAL:
                        dispatchScroll(activity, action == SCROLL_HORIZONTAL, first, second, third);
                        break;
                    default: throw new IllegalStateException("Invalid ART window input action " + action);
                }
                continue;
            }
            touches++;
            long now = android.os.SystemClock.uptimeMillis();
            if (action == android.view.MotionEvent.ACTION_DOWN) {
                touchDown = now;
                // A primary-button drag emulates a finger. Keep its identity through
                // UP/CANCEL, whose native button mask is already empty.
                touchGesture = (third & MotionEvent.BUTTON_PRIMARY) != 0;
            }
            boolean pressed = action == android.view.MotionEvent.ACTION_DOWN || action == android.view.MotionEvent.ACTION_MOVE;
            android.view.MotionEvent.PointerProperties pointer = new android.view.MotionEvent.PointerProperties();
            pointer.id = 0;
            pointer.toolType = touchGesture ? MotionEvent.TOOL_TYPE_FINGER : MotionEvent.TOOL_TYPE_MOUSE;
            android.view.MotionEvent.PointerCoords position = new android.view.MotionEvent.PointerCoords();
            position.x = first;
            position.y = second;
            position.pressure = pressed ? 1 : 0;
            position.size = 1;
            android.view.MotionEvent event = android.view.MotionEvent.obtain(touchDown, now, action, 1,
                new android.view.MotionEvent.PointerProperties[]{pointer}, new android.view.MotionEvent.PointerCoords[]{position},
                0, touchGesture ? 0 : third, 1, 1, 0, 0,
                touchGesture ? InputDevice.SOURCE_TOUCHSCREEN : InputDevice.SOURCE_MOUSE, 0);
            try { if (!ArtServices.windows.dispatchTouch(event)) activity.dispatchTouchEvent(event); }
            finally { event.recycle(); }
        }
        return touches;
    }

    private static void dispatchKey(Activity activity, KeyEvent event) {
        View child = ArtServices.windows.keyTarget();
        if (child == null) activity.dispatchKeyEvent(event);
        else child.dispatchKeyEvent(event);
    }

    private static void dispatchText(Activity activity, char character, ArtInputMethod.TextSession input) {
        if (Character.isHighSurrogate(character)) {
            highSurrogate = character;
            return;
        }
        String text = Character.isLowSurrogate(character)
            ? (highSurrogate == 0 ? "" : new String(new char[]{highSurrogate, character})) : String.valueOf(character);
        highSurrogate = 0;
        if (text.isEmpty()) return;
        View child = ArtServices.windows.keyTarget();
        View editor = child == null ? activity.getCurrentFocus() : child.findFocus();
        if (!input.commitText(editor, text))
            dispatchKey(activity, new KeyEvent(SystemClock.uptimeMillis(), text, KeyCharacterMap.VIRTUAL_KEYBOARD, 0));
    }

    private static void dispatchScroll(Activity activity, boolean horizontal, int x, int y, int delta) {
        MotionEvent.PointerProperties pointer = new MotionEvent.PointerProperties();
        pointer.id = 0;
        pointer.toolType = MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords position = new MotionEvent.PointerCoords();
        position.x = x;
        position.y = y;
        position.setAxisValue(horizontal ? MotionEvent.AXIS_HSCROLL : MotionEvent.AXIS_VSCROLL, delta / 120f);
        long now = SystemClock.uptimeMillis();
        MotionEvent event = MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1,
            new MotionEvent.PointerProperties[]{pointer}, new MotionEvent.PointerCoords[]{position},
            0, 0, 1, 1, 0, 0, InputDevice.SOURCE_MOUSE, 0);
        try {
            if (!ArtServices.windows.dispatchScroll(event)) activity.dispatchGenericMotionEvent(event);
        } finally { event.recycle(); }
    }

    static int captureProtection() { return nativeCaptureProtection(); }
    private static native int nativeCaptureProtection();
    private static native boolean nativePresent(int[] pixels, int width, int height, boolean secure);
    private static native int[] nativePollInput();
    private static native boolean nativeIsOpen();
    private static native void nativeOpen();
    private static native boolean nativeSetResizable(boolean enabled);
    private static native long nativeClientSize();
    private static native boolean nativeSetOrientation(boolean wide);
    private static native void nativeMinimize();
    private static native void nativeSetVisible(boolean visible);
    private static native boolean nativeSetIcon(byte[] png);
    private static native void nativeSetAppearance(String label, String path, String command);
    private static native void nativeSetAppUserModelId(String id);
    private static native boolean nativeClose();
    private static void mainThread() {
        if (android.os.Looper.myLooper() == null || android.os.Looper.myLooper() != android.os.Looper.getMainLooper())
            throw new IllegalStateException("ART window operations require the main Looper thread");
    }
}
