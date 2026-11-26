package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/** User-requested package removal through the local Activity result contract. */
final class ArtUninstaller {
    final ArtPackage apk;
    final ArtPackageStore packages;
    private volatile Context context;
    private boolean recovering;
    private final Handler main = new Handler(Looper.getMainLooper());

    ArtUninstaller(ArtPackage apk, ArtPackageStore packages) { this.apk = apk; this.packages = packages; }

    synchronized void attach(Context context) {
        this.context = context;
        if (context == null || recovering) return;
        recovering = true;
        worker(() -> {
            try {
                for (PackageInfo info : packages.pendingRemovals()) {
                    try {
                        packages.uninstall(info.packageName, info.applicationInfo.sourceDir);
                    } catch (Exception error) { Log.w("StoneUninstaller", "Could not recover pending removal: " + info.packageName, error); }
                }
            } catch (Exception error) { Log.w("StoneUninstaller", "Could not query pending removals", error); }
        });
    }

    ActivityInfo info(ComponentName component) {
        if (component == null || !apk.applicationInfo.packageName.equals(component.getPackageName())
                || !ArtUninstallActivity.class.getName().equals(component.getClassName())) return null;
        ActivityInfo info = new ActivityInfo(apk.activityInfo);
        info.name = ArtUninstallActivity.class.getName(); info.targetActivity = null;
        info.exported = false; info.configChanges = 0; info.labelRes = 0;
        info.nonLocalizedLabel = ArtText.get(ArtStrings.uninstall_title);
        boolean dark = (apk.resources.getConfiguration().uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK)
            == android.content.res.Configuration.UI_MODE_NIGHT_YES;
        info.theme = dark ? android.R.style.Theme_DeviceDefault : android.R.style.Theme_DeviceDefault_Light;
        return info;
    }

    ActivityInfo resolve(Intent intent) {
        if (packageName(intent) == null || intent.getPackage() != null && !apk.applicationInfo.packageName.equals(intent.getPackage())) return null;
        ComponentName component = intent.getComponent();
        if (component == null) component = new ComponentName(apk.applicationInfo.packageName, ArtUninstallActivity.class.getName());
        return info(component);
    }

    private static String packageName(Intent intent) {
        if (intent == null || !(Intent.ACTION_DELETE.equals(intent.getAction()) || Intent.ACTION_UNINSTALL_PACKAGE.equals(intent.getAction()))
                || intent.getData() == null || !"package".equals(intent.getData().getScheme()) || intent.getData().getFragment() != null) return null;
        String name = intent.getData().getSchemeSpecificPart();
        return name != null && name.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+") ? name : null;
    }

    Request begin(Intent intent) {
        String name = packageName(intent);
        if (name == null) throw new IllegalArgumentException("Invalid package removal request");
        String[] permissions = apk.packageInfo.requestedPermissions;
        if (apk.applicationInfo.targetSdkVersion >= 28 && (permissions == null
                || !java.util.Arrays.asList(permissions).contains("android.permission.REQUEST_DELETE_PACKAGES")))
            throw new SecurityException("Caller must declare REQUEST_DELETE_PACKAGES");
        if (intent.getBooleanExtra("android.intent.extra.UNINSTALL_ALL_USERS", false))
            throw new UnsupportedOperationException("All-user removal is not supported");
        if (intent.getBooleanExtra("android.intent.extra.KEEP_DATA", false))
            throw new UnsupportedOperationException("Keeping uninstalled application data is not supported");
        if (intent.hasExtra(Intent.EXTRA_USER)) throw new UnsupportedOperationException("Explicit uninstall users are not supported");
        Request request = new Request(name);
        worker(() -> {
            try {
                PackageInfo info = packages.removalInfo(name);
                if (info == null) throw new IllegalArgumentException("Package is not installed");
                if (apk.applicationInfo.packageName.equals(name)) throw new IllegalArgumentException("The running application is not a removable managed package");
                String label = name;
                try { label = ArtArchive.labelInLocale(info,
                    context.getResources().getConfiguration().getLocales().get(0).toLanguageTag()); }
                catch (Exception error) { Log.w("StoneUninstaller", "Could not load uninstall label", error); }
                synchronized (request) { if (request.cancelled) return; request.info = info; request.label = label; }
            } catch (Exception error) { synchronized (request) { request.error = error; } }
            request.changed();
        });
        return request;
    }

    private static void worker(Runnable action) {
        Thread thread = new Thread(action, "Stone APK removal"); thread.setDaemon(true); thread.start();
    }

    final class Request {
        final String packageName;
        PackageInfo info;
        String label;
        Exception error;
        boolean busy, completed, cancelled;
        private ArtSettingsConfirmation.Session nativeConfirmation;
        private ArtUninstallActivity activity;
        Request(String name) { packageName = name; }
        synchronized void attach(ArtUninstallActivity owner) { activity = owner; changed(); }
        synchronized void detach(ArtUninstallActivity owner) { if (activity == owner) activity = null; }
        synchronized boolean cancel() {
            if (busy || completed) return false;
            cancelled = true;
            if (nativeConfirmation != null) nativeConfirmation.cancel();
            return true;
        }
        synchronized void confirmInSettings() {
            if (nativeConfirmation != null || info == null || busy || completed || cancelled || error != null) return;
            try {
                nativeConfirmation = ArtSettingsConfirmation.create("uninstall", label == null ? packageName : label, packageName,
                    info.versionName, apk.applicationInfo.loadLabel(context.getPackageManager()).toString(), true);
                nativeConfirmation.attach(accepted -> {
                    synchronized (Request.this) {
                        if (cancelled || busy || completed) return;
                        if (accepted) approve(); else { cancel(); changed(); }
                    }
                });
            } catch (Exception failure) { Log.w("StoneSettings", "Could not request native removal confirmation", failure); cancel(); changed(); }
        }
        void changed() {
            main.post(() -> { synchronized (Request.this) {
                if (activity != null && !activity.isDestroyed() && !activity.isFinishing()) activity.render(Request.this);
            } });
        }
        synchronized void approve() {
            if (busy || completed || cancelled || info == null) return;
            busy = true; error = null; changed();
            worker(() -> {
                try {
                    packages.uninstall(packageName, info.applicationInfo.sourceDir);
                    synchronized (Request.this) { completed = true; busy = false; }
                } catch (Exception failure) {
                    Log.w("StoneUninstaller", "Package removal failed: " + packageName, failure);
                    synchronized (Request.this) { busy = false; error = failure; }
                }
                changed();
            });
        }
    }
}
