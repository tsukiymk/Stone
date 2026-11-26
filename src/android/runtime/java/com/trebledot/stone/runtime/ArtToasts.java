package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.Context;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;
import java.lang.reflect.Method;
import java.util.LinkedHashMap;

/** Foreground transient feedback, rendered by the image rather than Windows notification history. */
final class ArtToasts {
    private static final int MAX_PENDING = 5;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final LinkedHashMap<IBinder, Item> pending = new LinkedHashMap<>();
    private Item active;
    private static final class Item {
        final IBinder token, windowToken = new Binder();
        final Object service, callback;
        final CharSequence text;
        int duration;
        Object presenter;
        Runnable timeout;
        Item(IBinder token, Object service, Object callback, CharSequence text, int duration) {
            this.token = token; this.service = service; this.callback = callback; this.text = text; this.duration = duration;
        }
    }
    synchronized boolean enqueue(Object service, Object[] args, boolean text) {
        IBinder token = (IBinder) args[1];
        int duration = (Integer) args[3], display = (Integer) args[5];
        if (token == null || duration != Toast.LENGTH_SHORT && duration != Toast.LENGTH_LONG)
            throw new IllegalArgumentException("Invalid toast token or duration");
        Object callback = text ? args[6] : args[2];
        CharSequence message = text ? (CharSequence) args[2] : null;
        if (text && (message == null || message.length() > 4096) || !text && callback == null)
            throw new IllegalArgumentException("Invalid toast contents");
        if (display != 0 || ArtServices.activities.importance() > android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND)
            return false;
        Item existing = pending.get(token);
        if (existing != null) {
            existing.duration = duration;
            main.post(() -> { synchronized (this) { if (active == existing) schedule(existing); } });
            return true;
        }
        if (pending.size() >= MAX_PENDING) return false;
        CharSequence snapshot = message == null ? null : android.text.TextUtils.stringOrSpannedString(message);
        pending.put(token,new Item(token,service,callback,snapshot,duration));
        main.post(this::showNext);
        return true;
    }
    synchronized boolean allows(WindowManager.LayoutParams attrs) {
        int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        return active != null && active.windowToken == attrs.token && (attrs.flags & flags) == flags;
    }
    void cancel(IBinder token) {
        final Item item;
        synchronized (this) { item = pending.remove(token); }
        if (item != null) main.post(() -> { hide(item); showNext(); });
    }
    void close() {
        final Item item;
        synchronized (this) { pending.clear(); item = active; }
        if (item != null) hide(item);
    }
    private void showNext() {
        final Item item;
        synchronized (this) {
            if (active != null || pending.isEmpty()) return;
            if (ArtServices.activities.importance() > android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND) {
                pending.clear(); return;
            }
            active = item = pending.values().iterator().next();
        }
        try {
            if (item.text == null) {
                Class.forName("android.app.ITransientNotification").getMethod("show",IBinder.class).invoke(item.callback,item.windowToken);
            } else {
                Activity activity = ArtServices.activities.currentActivity(null);
                if (activity == null) throw new IllegalStateException("Toast has no visible application Activity");
                Context context = new ContextThemeWrapper(activity.getApplicationContext(),android.R.style.Theme_DeviceDefault_DayNight);
                Class<?> type = Class.forName("android.widget.ToastPresenter");
                Class<?> accessibility = Class.forName("android.view.accessibility.IAccessibilityManager");
                Class<?> notification = Class.forName("android.app.INotificationManager");
                IBinder binder = (IBinder) Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"accessibility");
                Object accessibilityService = Class.forName("android.view.accessibility.IAccessibilityManager$Stub")
                    .getMethod("asInterface",IBinder.class).invoke(null,binder);
                item.presenter = type.getConstructor(Context.class,accessibility,notification,String.class)
                    .newInstance(context,accessibilityService,item.service,activity.getPackageName());
                View view = (View) type.getMethod("getTextToastView",Context.class,CharSequence.class).invoke(null,context,item.text);
                int gravityId = context.getResources().getIdentifier("config_toastDefaultGravity","integer","android");
                int offsetId = context.getResources().getIdentifier("toast_y_offset","dimen","android");
                int gravity = gravityId == 0 ? Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL : context.getResources().getInteger(gravityId);
                int offset = offsetId == 0 ? Math.round(64 * context.getResources().getDisplayMetrics().density)
                    : context.getResources().getDimensionPixelSize(offsetId);
                type.getMethod("show",View.class,IBinder.class,IBinder.class,int.class,int.class,int.class,int.class,float.class,float.class,
                    Class.forName("android.app.ITransientNotificationCallback"))
                    .invoke(item.presenter,view,item.token,item.windowToken,item.duration,gravity,0,offset,0f,0f,item.callback);
            }
            synchronized (this) { if (active == item) schedule(item); }
        } catch (ReflectiveOperationException | RuntimeException | LinkageError error) {
            android.util.Log.w("StoneToast","Cannot display transient application feedback",error);
            synchronized (this) { pending.remove(item.token,item); }
            hide(item); main.post(this::showNext);
        }
    }
    private void schedule(Item item) {
        if (item.timeout != null) main.removeCallbacks(item.timeout);
        item.timeout = () -> { synchronized (this) { pending.remove(item.token,item); } hide(item); showNext(); };
        main.postDelayed(item.timeout,item.duration == Toast.LENGTH_LONG ? 3500 : 2000);
    }
    private void hide(Item item) {
        synchronized (this) { if (active != item) return; active = null; }
        if (item.timeout != null) main.removeCallbacks(item.timeout);
        try {
            if (item.presenter != null) {
                Class<?> type = item.presenter.getClass();
                if (type.getMethod("getView").invoke(item.presenter) != null)
                    type.getMethod("hide",Class.forName("android.app.ITransientNotificationCallback")).invoke(item.presenter,item.callback);
            } else if (item.text == null) {
                Class.forName("android.app.ITransientNotification").getMethod("hide").invoke(item.callback);
            }
        } catch (ReflectiveOperationException | RuntimeException | LinkageError error) {
            android.util.Log.w("StoneToast","Cannot hide transient application feedback",error);
        }
    }
}
