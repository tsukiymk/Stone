package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.IBinder;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/** Local task state for Activity tokens owned by this Windows process. */
final class ArtActivities {
    private static final class Record {
        volatile int requestedOrientation;
        final ComponentName component;
        final IBinder resultTo;
        Object description;
        Record(ActivityInfo info, IBinder caller, int request) {
            requestedOrientation = info.screenOrientation;
            component = new ComponentName(info.packageName,info.name);
            resultTo = request >= 0 ? caller : null;
        }
    }
    private final Map<IBinder, Record> records = Collections.synchronizedMap(new HashMap<>());
    private volatile ArtApplication application;
    private ArtCrossActivities cross;
    boolean hasExternalWork() { return cross != null && cross.hasWork(); }
    void attachExternal(IBinder token, String route, ComponentName caller) { cross.attach(token,route,caller); }
    void finishExternal(IBinder token, int result, Intent data) { if (cross != null) cross.finish(token,result,data); }

    int importance() {
        ArtApplication current = application;
        return current == null ? android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED : current.activityImportance();
    }

    void register(IBinder token, ActivityInfo info, IBinder caller, int request) {
        records.putIfAbsent(token, new Record(info,caller,request));
    }
    void unregister(IBinder token, boolean recreating) {
        if (!recreating) records.remove(token);
        if (!recreating && cross != null) cross.removed(token);
    }
    void setApplication(ArtApplication application) {
        if (cross != null) cross.close();
        cross = null;
        this.application = application;
        if (application != null) cross = new ArtCrossActivities(application);
        if (ArtServices.broadcasts != null) ArtServices.broadcasts.attach(application);
        if (ArtServices.installer != null) ArtServices.installer.attach(application == null ? null : application.context);
        if (ArtServices.serviceHost != null) ArtServices.serviceHost.attach(application);
        if (ArtServices.jobs != null) ArtServices.jobs.attach(application);
        if (ArtServices.uninstaller != null) ArtServices.uninstaller.attach(application == null ? null : application.context);
    }
    void configurationChanged(android.content.res.Configuration config, int changes) {
        if (application != null) application.postConfiguration(config, changes);
    }

    Activity currentActivity(Activity fallback) {
        return application == null ? fallback : application.currentActivity(fallback);
    }
    Intent retainedIntent(Activity activity) {
        try { return application == null ? activity.getIntent() : application.retainedIntent(activity); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Cannot read retained launch Intent",error); }
    }

    int start(String packageName, Intent intent, IBinder caller, String who, int request) {
        if (Boolean.getBoolean("stone.diagnostics")) android.util.Log.i("StoneActivity","Start " + intent + " request=" + request);
        if (application == null || !application.apk.applicationInfo.packageName.equals(packageName)) return -94;
        if (intent != null && intent.getComponent() != null && intent.getPackage() != null
                && !intent.getPackage().equals(intent.getComponent().getPackageName())) return -91;
        ActivityInfo target = ArtPermissions.resolve(application.apk, intent);
        if (target != null && (caller == null || !records.containsKey(caller))) return -94;
        if (target == null) {
            target = ArtLocaleSettings.resolve(application.apk,intent);
            if (target != null && caller != null && !records.containsKey(caller)) return -94;
        }
        if (target == null) {
            target = ArtStorageAccess.resolve(application.apk,intent);
            if (target != null && (caller == null || !records.containsKey(caller))) return -94;
        }
        if (target == null) {
            target = ArtDocuments.resolve(application.apk,intent);
            if (target != null && (caller == null || !records.containsKey(caller) || request < 0)) return -94;
        }
        if (target == null && ArtServices.installer != null) target = ArtServices.installer.resolveConfirmation(intent);
        if (target == null && ArtServices.uninstaller != null) target = ArtServices.uninstaller.resolve(intent);
        if (target == null) target = application.apk.resolveActivity(intent,android.content.pm.PackageManager.MATCH_DEFAULT_ONLY);
        if (target == null && ArtWebLinks.resolve(intent) != null) return ArtWebLinks.start(application, intent, request);
        if (target == null && ArtShares.resolve(intent) != null) return ArtShares.start(application, intent, request);
        if (target == null && intent != null && intent.getComponent() == null && cross != null && cross.enabled()) {
            android.content.pm.ResolveInfo resolved = application.context.getPackageManager().resolveActivity(intent,android.content.pm.PackageManager.MATCH_DEFAULT_ONLY);
            if (resolved != null && resolved.activityInfo != null)
                intent = new Intent(intent).setComponent(new ComponentName(resolved.activityInfo.packageName,resolved.activityInfo.name));
        }
        if (target == null && intent != null && intent.getComponent() != null
                && !application.apk.applicationInfo.packageName.equals(intent.getComponent().getPackageName())) {
            if (cross != null && cross.enabled() && !(request < 0 && Intent.ACTION_MAIN.equals(intent.getAction()) && intent.hasCategory(Intent.CATEGORY_LAUNCHER))) {
                Record source = records.get(caller);
                if (request >= 0 && source == null) return -94;
                ComponentName component = source == null
                    ? new ComponentName(packageName,application.apk.activityInfo.name) : source.component;
                return cross.start(intent,caller,who,request,component);
            }
            return ArtApplicationLauncher.start(application, intent, request);
        }
        if (target == null) return intent.getComponent() == null ? -91 : -92;
        if (!ArtProcess.current(application.apk).equals(target.processName) && !target.name.startsWith("com.trebledot.stone.runtime.")) {
            Record source = records.get(caller);
            if (cross == null || !cross.enabled() || source == null) return -94;
            return cross.startLocal(new Intent(intent).setComponent(new ComponentName(target.packageName,target.name)),
                target,caller,who,request,source.component);
        }
        return application.postLaunch(target, intent, caller, who, request) ? 0 : -96;
    }

    Object call(Object instance, Method method, Object[] arguments) {
        Record record = arguments != null && arguments.length > 0 ? records.get(arguments[0]) : null;
        // Android identifies only the Activity awaiting this result. An ordinary
        // start or an unknown/finished token has no result caller. Never use
        // caller-supplied Intent extras or its mutable client-side Intent.
        if (method.getName().equals("getCallingActivity") || method.getName().equals("getCallingPackage")) {
            Record caller = record == null ? null : records.get(record.resultTo);
            ComponentName component = caller != null ? caller.component : record == null || cross == null ? null : cross.caller((IBinder)arguments[0]);
            return component == null ? null : method.getName().equals("getCallingActivity") ? component : component.getPackageName();
        }
        if (record == null) throw new IllegalArgumentException("Unknown ART Activity token: " + method.getName());
        switch (method.getName()) {
            // The software host switches attached roots immediately; it has no
            // Android window-transition animation/compositor to override.
            case "overridePendingTransition": return null;
            case "setTaskDescription": record.description = arguments[1]; return null;
            case "getTaskForActivity": return 1;
            case "finishActivity":
                Intent result = cross == null ? (Intent)arguments[2] : cross.copyResult((IBinder)arguments[0],(Intent)arguments[2]);
                return application.postFinish((IBinder) arguments[0], (Integer) arguments[1], result);
            case "isTopOfTask": return application.isTop((IBinder) arguments[0]);
            case "onBackPressed":
                if (application.hasPreviousActivity() || record.resultTo != null
                        || (cross != null && cross.isIncoming((IBinder)arguments[0]))) {
                    // Let the image Activity finish itself, preserving setResult,
                    // finishAfterTransition and the framework's finishing state.
                    try { method.getParameterTypes()[1].getMethod("requestFinish").invoke(arguments[1]); }
                    catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not request Activity finish after Back",error); }
                } else ArtWindow.minimize();
                return null;
            case "getDisplayId": return 0;
            // Orientation requests belong to the logical Activity, including
            // calls from native render threads and configuration recreation.
            // The main window loop applies fixed-axis requests and dispatches
            // the resulting ordinary resize/configuration lifecycle.
            case "setRequestedOrientation": record.requestedOrientation = (Integer) arguments[1]; return null;
            case "getRequestedOrientation": return record.requestedOrientation;
            case "isInMultiWindowMode": return ArtConfiguration.resizableWindow();
            case "isInPictureInPictureMode": return false;
            default: throw new UnsupportedOperationException("ART activity client: " + method.getName());
        }
    }
}
