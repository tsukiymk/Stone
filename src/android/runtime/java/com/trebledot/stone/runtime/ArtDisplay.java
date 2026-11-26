package com.trebledot.stone.runtime;

import android.graphics.Point;
import android.view.Display;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/** Supplies the host-backed application display to the image's DisplayManager. */
final class ArtDisplay {
    private static native int[] readMetrics();
    private final Object info;
    private final Object overlays;
    private int width;
    private int height;
    private final int uid;
    private int density;
    private final float animationScale;
    private final List<Object> callbacks = new ArrayList<>();
    private Object windowSession;

    ArtDisplay(int uid) throws ReflectiveOperationException {
        int[] metrics = readMetrics();
        if (metrics.length != 6 || metrics[0] <= 0 || metrics[1] <= 0 || metrics[2] <= 0)
            throw new IllegalStateException("Invalid Windows display metrics");
        width = metrics[0];
        height = metrics[1];
        this.uid = uid;
        density = metrics[2];
        animationScale = metrics[5] != 0 ? 1.0f : 0.0f;
        Class<?> type = Class.forName("android.view.DisplayInfo");
        info = type.getConstructor().newInstance();
        set("displayId", 0);
        set("name", "Stone Windows display");
        set("uniqueId", "stone:windows:primary");
        set("appWidth", width);
        set("appHeight", height);
        set("logicalWidth", width);
        set("logicalHeight", height);
        set("smallestNominalAppWidth", width);
        set("smallestNominalAppHeight", height);
        set("largestNominalAppWidth", width);
        set("largestNominalAppHeight", height);
        set("logicalDensityDpi", metrics[2]);
        set("physicalXDpi", (float) metrics[3]);
        set("physicalYDpi", (float) metrics[3]);
        set("type", 1); // Display.TYPE_INTERNAL
        set("state", Display.STATE_ON);
        // This host exposes no audio output, including platform touch sounds.
        Field touchFeedback = Display.class.getDeclaredField("FLAG_TOUCH_FEEDBACK_DISABLED");
        touchFeedback.setAccessible(true);
        set("flags", touchFeedback.getInt(null));
        set("modeId", 1);
        set("defaultModeId", 1);
        set("renderFrameRate", (float) metrics[4]);
        Constructor<Display.Mode> mode = Display.Mode.class.getDeclaredConstructor(int.class, int.class, int.class, float.class);
        mode.setAccessible(true);
        set("supportedModes", new Display.Mode[]{mode.newInstance(1, width, height, (float) metrics[4])});
        set("supportedColorModes", new int[]{0});
        Class<?> overlayType = Class.forName("android.hardware.OverlayProperties");
        Constructor<?> constructor = overlayType.getDeclaredConstructor(long.class);
        constructor.setAccessible(true);
        overlays = constructor.newInstance(0L); // No hardware overlay planes exposed by this backend.
    }

    synchronized Object windowCall(Object instance, Method method, Object[] arguments) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "openSession": return windowSession;
            case "getCurrentAnimatorScale": return animationScale;
            case "hasNavigationBar": return false;
            case "getWindowInsets": {
                if (((Integer) arguments[0]) != 0) throw new IllegalArgumentException("Unknown Windows display");
                Object state = arguments[arguments.length - 1];
                state.getClass().getMethod("setDisplayFrame", android.graphics.Rect.class)
                    .invoke(state,new android.graphics.Rect(0,0,width,height));
                // The Win32 software client has no Android status/navigation bars.
                return false;
            }
            case "getInitialDisplaySize":
            case "getBaseDisplaySize":
                if (((Integer) arguments[0]) != 0) throw new IllegalArgumentException("Unknown Windows display");
                ((Point) arguments[1]).set(width, height);
                return null;
            case "getInitialDisplayDensity":
            case "getBaseDisplayDensity": return ((Integer) arguments[0]) == 0 ? density : -1;
            default: throw new UnsupportedOperationException("ART window service: " + method.getName());
        }
    }

    float animationScale() { return animationScale; }
    synchronized void resize(int width, int height, int density) throws ReflectiveOperationException {
        if (this.width == width && this.height == height && this.density == density) return;
        this.width = width; this.height = height;
        this.density = density;
        set("logicalDensityDpi", density);
        set("physicalXDpi", density * (96.0f / 160.0f));
        set("physicalYDpi", density * (96.0f / 160.0f));
        for (String name : new String[]{"appWidth", "logicalWidth", "smallestNominalAppWidth", "largestNominalAppWidth"}) set(name, width);
        for (String name : new String[]{"appHeight", "logicalHeight", "smallestNominalAppHeight", "largestNominalAppHeight"}) set(name, height);
        Class.forName("android.hardware.display.DisplayManagerGlobal").getMethod("invalidateLocalDisplayInfoCaches").invoke(null);
        for (Object callback : new ArrayList<>(callbacks)) {
            Class.forName("android.hardware.display.IDisplayManagerCallback").getMethod("onDisplayEvent", int.class, int.class)
                .invoke(callback, 0, 2); // DisplayManagerGlobal.EVENT_DISPLAY_CHANGED
        }
    }
    void setWindowSession(Object session) { windowSession = session; }

    synchronized Object call(Object instance, Method method, Object[] arguments) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "getDisplayInfo": return ((Integer) arguments[0]) == 0
                ? info.getClass().getConstructor(info.getClass()).newInstance(info) : null;
            case "getDisplayIds": return new int[]{0};
            case "getStableDisplaySize": return new Point(width, height);
            case "getPreferredWideGamutColorSpaceId": return 0; // sRGB
            case "getOverlaySupport": return overlays;
            case "registerCallbackWithEventMask":
                if (!callbacks.contains(arguments[0])) callbacks.add(arguments[0]);
                return null;
            case "isUidPresentOnDisplay": return ((Integer) arguments[0]) == uid && ((Integer) arguments[1]) == 0;
            case "getDisplayDecorationSupport": return null;
            default: throw new UnsupportedOperationException("ART display service: " + method.getName());
        }
    }

    private void set(String name, Object value) throws ReflectiveOperationException {
        Field field = info.getClass().getField(name);
        field.set(info, value);
    }
}
