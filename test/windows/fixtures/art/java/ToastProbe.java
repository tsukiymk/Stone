package com.trebledot.stone.probe;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Binder;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.Toast;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** Real image ToastPresenter, Binder callbacks, pixels, timing and token isolation. */
final class ToastProbe {
    private final Object windows;
    private final Method draw;
    private final java.lang.reflect.Field children;
    private final Bitmap bitmap = Bitmap.createBitmap(480,800,Bitmap.Config.ARGB_8888);
    private ToastProbe() throws Exception {
        Class<?> services = Class.forName("com.trebledot.stone.runtime.ArtServices");
        java.lang.reflect.Field field = services.getDeclaredField("windows"); field.setAccessible(true); windows = field.get(null);
        draw = windows.getClass().getDeclaredMethod("draw",Canvas.class,int.class,int.class,
            Class.forName("com.trebledot.stone.runtime.ArtSoftwareRipples")); draw.setAccessible(true);
        children = windows.getClass().getDeclaredField("children"); children.setAccessible(true);
    }
    static void verify(Activity activity) throws Exception {
        ToastProbe probe = new ToastProbe();
        try { probe.run(activity); }
        finally { probe.bitmap.recycle(); }
    }
    private int count() throws Exception { return ((Map<?,?>) children.get(windows)).size(); }
    private void frame() throws Exception {
        BroadcastProbe.drain(); bitmap.eraseColor(0xff446688); Canvas canvas = new Canvas(bitmap);
        try { draw.invoke(windows,canvas,480,800,null); } finally { canvas.setBitmap(null); }
    }
    private void until(String stage, java.util.concurrent.Callable<Boolean> ready, long timeout) throws Exception {
        long end = SystemClock.uptimeMillis() + timeout;
        do { frame(); if (ready.call()) return; Thread.sleep(5); } while (SystemClock.uptimeMillis() < end);
        throw new AssertionError("Toast did not reach " + stage);
    }
    private static Toast.Callback callback(List<String> events) {
        return new Toast.Callback() {
            @Override public void onToastShown() { events.add("shown"); }
            @Override public void onToastHidden() { events.add("hidden"); }
        };
    }
    private static TextView text(View view, String value) {
        if (view instanceof TextView && value.contentEquals(((TextView) view).getText())) return (TextView) view;
        if (view instanceof android.view.ViewGroup) for (int index = 0; index < ((android.view.ViewGroup) view).getChildCount(); index++) {
            TextView found = text(((android.view.ViewGroup) view).getChildAt(index),value); if (found != null) return found;
        }
        return null;
    }
    private void run(Activity activity) throws Exception {
        if (count() != 0) throw new AssertionError("Toast fixture has unrelated child windows");
        List<String> shortEvents = new ArrayList<>();
        android.text.SpannableStringBuilder message = new android.text.SpannableStringBuilder("Stone transient 提示");
        Toast shortToast = Toast.makeText(activity.getApplicationContext(),message,Toast.LENGTH_SHORT);
        shortToast.addCallback(callback(shortEvents)); shortToast.show(); message.replace(0,message.length(),"mutated");
        until("short text shown",() -> shortEvents.equals(java.util.Arrays.asList("shown")) && count() == 1,5000);
        Object child = ((Map<?,?>) children.get(windows)).values().iterator().next();
        java.lang.reflect.Field childView = child.getClass().getDeclaredField("view"); childView.setAccessible(true);
        View view = (View) childView.get(child);
        if (text(view,"Stone transient 提示") == null || !view.isAttachedToWindow()) throw new AssertionError("Mutable toast contents were not isolated");
        int[] pixels = new int[480 * 800]; bitmap.getPixels(pixels,0,480,0,0,480,800);
        if (java.util.Arrays.stream(pixels).filter(color -> color != 0xff446688).count() < 64)
            throw new AssertionError("Image toast did not paint actual pixels");
        WindowManager.LayoutParams attrs = (WindowManager.LayoutParams) view.getLayoutParams();
        int protectedFlags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        if (attrs.type != WindowManager.LayoutParams.TYPE_TOAST || (attrs.flags & protectedFlags) != protectedFlags)
            throw new AssertionError("Toast became an interactive application overlay");
        Method touch = windows.getClass().getDeclaredMethod("dispatchTouch",MotionEvent.class); touch.setAccessible(true);
        long now = SystemClock.uptimeMillis(); MotionEvent down = MotionEvent.obtain(now,now,MotionEvent.ACTION_DOWN,240,700,0);
        try { if ((Boolean) touch.invoke(windows,down)) throw new AssertionError("Toast intercepted input"); } finally { down.recycle(); }
        until("short automatic hide",() -> shortEvents.equals(java.util.Arrays.asList("shown","hidden")) && count() == 0,4000);

        List<String> longEvents = new ArrayList<>();
        Toast longToast = Toast.makeText(activity,"Long feedback",Toast.LENGTH_LONG); longToast.addCallback(callback(longEvents));
        longToast.show(); until("long shown",() -> longEvents.size() == 1 && count() == 1,5000);
        long keepUntil = SystemClock.uptimeMillis() + 2150;
        do { frame(); Thread.sleep(5); } while (SystemClock.uptimeMillis() < keepUntil);
        if (!longEvents.equals(java.util.Arrays.asList("shown")) || count() != 1) throw new AssertionError("Long feedback used the short timeout");
        List<String> cancelledEvents = new ArrayList<>();
        Toast queued = Toast.makeText(activity,"Cancelled before display",Toast.LENGTH_SHORT);
        queued.addCallback(callback(cancelledEvents)); queued.show(); queued.cancel(); longToast.cancel();
        until("cancelled",() -> count() == 0 && longEvents.equals(java.util.Arrays.asList("shown","hidden")),5000);
        if (!cancelledEvents.isEmpty()) throw new AssertionError("Cancelled queued toast was displayed");

        TextView customView = new TextView(activity); customView.setText("Custom image ToastPresenter");
        Toast custom = new Toast(activity); custom.setView(customView); custom.setDuration(Toast.LENGTH_LONG);
        List<String> customEvents = new ArrayList<>(); custom.addCallback(callback(customEvents)); custom.show();
        until("custom shown",() -> customView.isAttachedToWindow() && customEvents.equals(java.util.Arrays.asList("shown")),5000);
        custom.cancel(); until("custom hidden",() -> !customView.isAttachedToWindow() && customEvents.equals(java.util.Arrays.asList("shown","hidden")),5000);

        TextView forged = new TextView(activity); forged.setText("Unowned toast token");
        WindowManager.LayoutParams invalid = new WindowManager.LayoutParams(-2,-2,WindowManager.LayoutParams.TYPE_TOAST,protectedFlags,-3);
        invalid.token = new Binder();
        try { activity.getWindowManager().addView(forged,invalid); throw new AssertionError("Unowned toast token accepted"); }
        catch (WindowManager.BadTokenException expected) { }
        if (count() != 0 || forged.isAttachedToWindow()) throw new AssertionError("Rejected toast left a window behind");

        List<Toast> burst = new ArrayList<>(); List<List<String>> burstEvents = new ArrayList<>();
        for (int index = 0; index < 6; index++) {
            List<String> events = new ArrayList<>(); burstEvents.add(events);
            Toast next = Toast.makeText(activity,"Queued feedback " + index,Toast.LENGTH_LONG);
            next.addCallback(callback(events)); burst.add(next); next.show();
        }
        until("bounded queue first visible",() -> burstEvents.get(0).equals(java.util.Arrays.asList("shown")) && count() == 1,5000);
        Class<?> services = Class.forName("com.trebledot.stone.runtime.ArtServices");
        java.lang.reflect.Field notifications = services.getDeclaredField("notifications"); notifications.setAccessible(true);
        Object manager = notifications.get(null);
        java.lang.reflect.Field toasts = manager.getClass().getDeclaredField("toasts"); toasts.setAccessible(true);
        Object host = toasts.get(manager);
        java.lang.reflect.Field pending = host.getClass().getDeclaredField("pending"); pending.setAccessible(true);
        if (((Map<?,?>) pending.get(host)).size() != 5) throw new AssertionError("Toast queue did not enforce its bounded capacity");
        burst.get(0).show(); BroadcastProbe.drain();
        if (((Map<?,?>) pending.get(host)).size() != 5 || !burstEvents.get(0).equals(java.util.Arrays.asList("shown")))
            throw new AssertionError("Showing the same token duplicated the toast");
        Method close = windows.getClass().getDeclaredMethod("close"); close.setAccessible(true); close.invoke(windows);
        until("window close cleanup",() -> count() == 0 && burstEvents.get(0).equals(java.util.Arrays.asList("shown","hidden")),5000);
        if (!((Map<?,?>) pending.get(host)).isEmpty() || burstEvents.subList(1,6).stream().anyMatch(events -> !events.isEmpty()))
            throw new AssertionError("Window closure retained queued feedback or displayed a cancelled toast");
        for (Toast toast : burst) toast.cancel(); BroadcastProbe.drain();
        System.out.println("PASS real image text/custom toasts, immutable text, pixels, noninteractive windows, short/long timing, callbacks, queued cancellation, capacity, duplicate tokens, window cleanup and token rejection.");
    }
}
