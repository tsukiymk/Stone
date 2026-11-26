package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.lang.reflect.Method;

/** Real manifest policy, Win32 sizing and image Activity configuration coverage. */
public final class ResizeWindowProbe {
    private static native long resizeWindow(int width, int height, int command);
    private static native boolean windowResizable();
    private static native boolean inspectResizeMask(int stage);
    private static native int changeWindowDpi(int dpi);
    private static native void clickWindow(int x, int y);
    private static TestActivity current;
    private static int creates, callbacks, surfaceWidth, surfaceHeight, canvasDensity;
    private static float touchX, touchY;
    private static final int SIZE_CHANGES = ActivityInfo.CONFIG_SCREEN_SIZE | ActivityInfo.CONFIG_SMALLEST_SCREEN_SIZE
        | ActivityInfo.CONFIG_ORIENTATION | ActivityInfo.CONFIG_SCREEN_LAYOUT;
    public static final class TestActivity extends Activity {
        int value;
        @Override public void onCreate(Bundle state) {
            super.onCreate(state); current = this; creates++;
            value = state == null ? 42 : state.getInt("value");
            SurfaceView view = new SurfaceView(this) {
                @Override protected void onDraw(android.graphics.Canvas canvas) { canvasDensity = canvas.getDensity(); }
            };
            view.setWillNotDraw(false);
            view.setOnTouchListener((target, event) -> { touchX = event.getX(); touchY = event.getY(); return true; });
            view.getHolder().addCallback(new SurfaceHolder.Callback() {
                public void surfaceCreated(SurfaceHolder holder) {}
                public void surfaceDestroyed(SurfaceHolder holder) {}
                public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                    surfaceWidth = width; surfaceHeight = height;
                }
            });
            setContentView(view);
        }
        @Override public void onSaveInstanceState(Bundle state) { super.onSaveInstanceState(state); state.putInt("value",value); }
        @Override public void onConfigurationChanged(Configuration config) { super.onConfigurationChanged(config); callbacks++; }
    }
    private static final class Drained extends RuntimeException {}
    private static void drain() {
        Drained done = new Drained();
        new Handler(Looper.getMainLooper()).post(() -> { throw done; });
        try { Looper.loop(); } catch (Drained signal) { if (signal != done) throw signal; }
    }
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }

    public static int run(AssetManager assets, String unused) throws Exception {
        String[] fixtures = {"default-23", "default-24", "app-false", "activity-true", "activity-false", "resource-true", "alias"};
        for (int index = 0; index < fixtures.length; ++index) {
            java.lang.reflect.Constructor<AssetManager> ctor = AssetManager.class.getDeclaredConstructor(); ctor.setAccessible(true);
            try (ArtPackage apk = ArtPackage.open(ctor.newInstance(), "/data/resize-fixtures/" + fixtures[index] + ".apk", "/data/resize-policy/" + index)) {
                Method policy = ArtPackage.class.getDeclaredMethod("resizableWindow"); policy.setAccessible(true);
                check((Boolean)policy.invoke(apk) == (index == 1 || index == 3 || index >= 5), "manifest resize policy: " + fixtures[index]);
            }
        }
        System.out.println("PASS resize manifest: target SDK, application inheritance, activity override, boolean resource and launcher alias");
        ArtPackage apk = ArtPackage.openApplication(assets,"/data/application.apk");
        ArtApplication app = ArtApplication.start(apk);
        check(apk.resources.getDisplayMetrics().densityDpi == changeWindowDpi(0),"initial resource density follows Windows");
        ActivityInfo info = new ActivityInfo(apk.activityInfo);
        info.name = TestActivity.class.getName(); info.targetActivity = null; info.configChanges = SIZE_CHANGES | ActivityInfo.CONFIG_DENSITY;
        Method launch = ArtApplication.class.getDeclaredMethod("launch", ActivityInfo.class, Intent.class, IBinder.class, String.class, int.class);
        launch.setAccessible(true);
        launch.invoke(app,info,new Intent().setClassName(app.context.getPackageName(),info.name),null,null,-1);
        float density = current.getResources().getDisplayMetrics().density;
        Class<?> loopType = Class.forName("com.trebledot.stone.runtime.ArtWindowLoop");
        java.lang.reflect.Constructor<?> constructor = loopType.getDeclaredConstructor(Activity.class,int.class,int.class); constructor.setAccessible(true);
        Object loop = constructor.newInstance(current,Math.round(360*density),Math.round(640*density));
        Method frame = loopType.getDeclaredMethod("nextFrame"); frame.setAccessible(true);
        Method start = loopType.getDeclaredMethod("start",Runnable.class); start.setAccessible(true);
        try {
            start.invoke(loop,new Object[]{null});
            check(windowResizable() && current.isInMultiWindowMode(),"resizable Win32 style and Android freeform mode");
            resizeMask(loop,frame);
            resize(loop,frame,800,480,0);
            check(callbacks > 0 && creates == 1,"handled resize configuration callback");
            resize(loop,frame,0,0,1); // Maximize and restore through the window thread.
            resize(loop,frame,0,0,2);
            resize(loop,frame,420,720,0);
            current.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            frame.invoke(loop); drain(); frame.invoke(loop);
            check(current.getResources().getConfiguration().orientation==Configuration.ORIENTATION_LANDSCAPE,
                "Activity landscape request did not rotate the actual window/resources");
            current.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
            frame.invoke(loop); drain(); frame.invoke(loop);
            check(current.getResources().getConfiguration().orientation==Configuration.ORIENTATION_PORTRAIT,
                "Clearing Activity orientation did not restore the user axis");
            System.out.println("PASS Activity fixed-axis window rotation and restoration with actual image configuration");
            dpi(loop,frame);
            current.value = 73; info.configChanges = 0;
            TestActivity previous = current;
            resize(loop,frame,760,460,0);
            check(current != previous && current.value == 73 && creates == 2,"resize recreation and saved Activity state");
            java.lang.reflect.Field mode = ActivityInfo.class.getField("resizeMode");
            ActivityInfo launcher = null;
            for (ActivityInfo candidate : apk.packageInfo.activities) if (candidate.name.equals(apk.activityInfo.name)) launcher = candidate;
            check(launcher != null,"launcher policy record");
            int originalMode = mode.getInt(launcher);
            Method open = ArtWindow.class.getDeclaredMethod("open", Activity.class); open.setAccessible(true);
            try {
                mode.setInt(launcher,ActivityInfo.class.getField("RESIZE_MODE_UNRESIZEABLE").getInt(null));
                open.invoke(null,current);
                check(!windowResizable(),"fixed-window application style");
            } finally { mode.setInt(launcher,originalMode); open.invoke(null,current); }
            check(windowResizable(),"resize policy restoration");
            System.out.println("PASS Win32 resize/maximize/restore, image resources and decor, native SurfaceView geometry, callbacks and saved-state recreation");
        } finally { ((AutoCloseable)loop).close(); app.destroyActivity(); }
        Method configuration = Class.forName("com.trebledot.stone.probe.ConfigurationProbe").getDeclaredMethod("run",ArtApplication.class);
        configuration.setAccessible(true); configuration.invoke(null,app);
        return 42;
    }
    private static void solidFrame(int width, int height) {
        int[] pixels = new int[width*height]; java.util.Arrays.fill(pixels,0xff197345);
        check(ArtWindow.present(pixels,width,height),"solid resize control frame");
    }
    private static void dpi(Object loop, Method frame) throws Exception {
        int originalDensity = changeWindowDpi(0);
        int originalDpi = Math.round(originalDensity * 96 / 160.0f);
        try {
            for (int dpi : new int[]{96,120,144,192}) {
                int previousCallbacks = callbacks;
                int previousDensity = changeWindowDpi(0);
                int expected = changeWindowDpi(dpi);
                // Keep the same client pixels: density changes alone must update resources.
                frame.invoke(loop); drain(); frame.invoke(loop); drain();
                android.util.DisplayMetrics metrics = current.getResources().getDisplayMetrics();
                Configuration config = current.getResources().getConfiguration();
                check(expected == Math.round(160 * dpi / 96.0f) && metrics.densityDpi == expected
                    && config.densityDpi == expected && metrics.density == dpi / 96.0f,"resource density at Windows DPI " + dpi);
                android.util.DisplayMetrics display = new android.util.DisplayMetrics();
                current.getDisplay().getMetrics(display);
                check(display.densityDpi == expected && display.density == metrics.density,"display density cache at DPI " + dpi);
                check(canvasDensity == expected,"software drawing Canvas density at DPI " + dpi);
                android.graphics.Bitmap glyph=android.graphics.Bitmap.createBitmap(8,8,android.graphics.Bitmap.Config.ARGB_8888);
                android.graphics.Bitmap snapshot=android.graphics.Bitmap.createBitmap(32,32,android.graphics.Bitmap.Config.ARGB_8888);
                android.graphics.Canvas glyphCanvas=new android.graphics.Canvas(snapshot);
                try {
                    check(glyph.getDensity()==expected,"cached text bitmap density at Windows DPI "+dpi);
                    glyph.eraseColor(0xff00ff00); glyphCanvas.setDensity(expected); glyphCanvas.drawBitmap(glyph,0,0,null);
                    check(snapshot.getPixel(7,7)==0xff00ff00 && snapshot.getPixel(8,7)==0,
                        "cached text bitmap was scaled a second time at Windows DPI "+dpi);
                } finally { glyphCanvas.setBitmap(null); glyph.recycle(); snapshot.recycle(); }
                check(config.screenWidthDp == (int)(metrics.widthPixels/metrics.density)
                    && config.screenHeightDp == (int)(metrics.heightPixels/metrics.density),"dp geometry at DPI " + dpi);
                check(creates == 1 && (previousDensity == expected || callbacks > previousCallbacks),"handled density configuration callback");
                android.widget.TextView text = new android.widget.TextView(current);
                text.setTextSize(android.util.TypedValue.COMPLEX_UNIT_DIP,16);
                check(Math.abs(text.getTextSize()-16*dpi/96.0f) < 0.01f,"toolkit text size follows density");
                touchX = touchY = -1;
                clickWindow(72,120); ArtWindow.pollTouches(current);
                check(Math.abs(touchX-72) < 0.01f && Math.abs(touchY-120) < 0.01f,"physical input alignment at DPI " + dpi);
            }
        } finally {
            changeWindowDpi(originalDpi); frame.invoke(loop); drain(); frame.invoke(loop); drain();
        }
        System.out.println("PASS Windows 100/125/150/200% density, same-size updates, display cache, dp geometry, configuration callbacks and input coordinates (synthetic WM_DPICHANGED)");
    }
    private static void resizeMask(Object loop, Method frame) throws Exception {
        android.graphics.Bitmap icon = android.graphics.Bitmap.createBitmap(72,72,android.graphics.Bitmap.Config.ARGB_8888);
        icon.eraseColor(0xffee2200);
        java.io.ByteArrayOutputStream png = new java.io.ByteArrayOutputStream();
        check(icon.compress(android.graphics.Bitmap.CompressFormat.PNG,100,png),"resize icon fixture"); icon.recycle();
        Method updateIcon = ArtWindow.class.getDeclaredMethod("updateIcon",byte[].class); updateIcon.setAccessible(true);
        check((Boolean)updateIcon.invoke(null,png.toByteArray()),"resize icon publication");
        solidFrame(current.getWindow().getDecorView().getWidth(),current.getWindow().getDecorView().getHeight());
        check(inspectResizeMask(0),"visible ART control pixels before dragging");
        check(inspectResizeMask(1),"dragging must show dark background and centered application icon");
        check(resizeWindow(680,440,0) == 0,"dragging must defer ART buffer resize");
        check(inspectResizeMask(2),"icon mask must follow the resized client center");
        check(inspectResizeMask(3),"mask must remain after dragging until a matching frame");
        solidFrame(680,440);
        check(inspectResizeMask(4),"matching ART frame must replace the icon mask");
        resize(loop,frame,680,440,0);
        System.out.println("PASS resize mask pixels: ART visibility, dark background, centered app icon, drag deferral and matching-frame removal");
    }
    private static void resize(Object loop, Method frame, int width, int height, int command) throws Exception {
        long size = resizeWindow(width,height,command);
        width = (int)(size >>> 32); height = (int)size;
        check(width > 0 && height > 0,"Win32 resize request");
        frame.invoke(loop); drain(); frame.invoke(loop); drain();
        Configuration config = current.getResources().getConfiguration();
        android.util.DisplayMetrics metrics = current.getResources().getDisplayMetrics();
        check(current.getWindow().getDecorView().getWidth() == width && current.getWindow().getDecorView().getHeight() == height,"decor layout matches client");
        check(metrics.widthPixels == width && metrics.heightPixels == height,"resource pixels match client");
        check(config.screenWidthDp == (int)(width/metrics.density) && config.screenHeightDp == (int)(height/metrics.density),"resource dp match client");
        check(surfaceWidth == width && surfaceHeight == height,"SurfaceView buffer matches client");
        android.graphics.Point point = new android.graphics.Point();
        current.getDisplay().getSize(point);
        check(point.x == width && point.y == height,"display cache matches client");
    }
}
