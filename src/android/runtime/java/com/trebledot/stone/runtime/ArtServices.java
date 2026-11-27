package com.trebledot.stone.runtime;

import android.app.job.JobScheduler;
import android.content.pm.ActivityInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.SigningInfo;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IInterface;
import android.os.UserHandle;
import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.lang.reflect.InvocationHandler;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/** In-process framework services supplied by the Windows application host. */
final class ArtServices {
    static ArtBroadcasts broadcasts;
    static ArtPackageInstaller installer;
    static ArtServiceHost serviceHost;
    static ArtUninstaller uninstaller;
    static ArtPackageStore packages;
    static ArtNotifications notifications;
    static ArtIntents pendingIntents;
    static ArtPower power;
    static ArtDisplay display;
    static ArtJobs jobs;
    static ArtStorage storage;
    static ArtWebView webView;
    private static Object frameworkFeatureCache;
    private static List<android.content.pm.FeatureInfo> features;
    static final ArtActivities activities = new ArtActivities();
    static final ArtWindowSession windows = new ArtWindowSession();
    private ArtServices() {}

    @SuppressWarnings("unchecked")
    static void install(ArtPackage apk, ClassLoader classLoader) throws ReflectiveOperationException {
        ArtAppearance.install(apk);
        Class<?> manager = Class.forName("android.os.ServiceManager");
        Field cacheField = manager.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        Map<String, android.os.IBinder> cache = (Map<String, android.os.IBinder>) cacheField.get(null);
        if (cache.containsKey("activity")) throw new IllegalStateException("Activity service is already installed");
        installRegistry(cache);
        cache.put("location", service("android.location.ILocationManager", ArtLocation::call));
        webView = new ArtWebView();
        cache.put("webviewupdate", service("android.webkit.IWebViewUpdateService", webView::call));
        cache.put("appwidget", service("com.android.internal.appwidget.IAppWidgetService", (instance, method, arguments) -> {
            // Stone has no Android widget host or bound widget instances.
            switch (method.getName()) {
                case "getMaxBitmapMemory": return 0;
                case "notifyProviderInheritance": return null;
                case "getAppWidgetIds": return new int[0];
                case "getInstalledProvidersForProfile":
                    return method.getReturnType().getConstructor(List.class).newInstance(Collections.emptyList());
                case "isBoundWidgetPackage":
                case "isRequestPinAppWidgetSupported":
                case "requestPinAppWidget": return false;
                default: throw new UnsupportedOperationException("ART app widget service: " + method.getName());
            }
        }));
        ArtAudio audio=new ArtAudio();
        cache.put("audio",service("android.media.IAudioService",audio::call));
        boolean hasWifi = ArtWifi.available();
        List<android.content.pm.FeatureInfo> availableFeatures=new ArrayList<>(Arrays.asList(ArtWifi.features(hasWifi)));
        features=availableFeatures;
        boolean hasAudio=ArtAudio.available();
        if(hasAudio) {
            android.content.pm.FeatureInfo info=new android.content.pm.FeatureInfo();
            info.name=PackageManager.FEATURE_AUDIO_OUTPUT; availableFeatures.add(info);
        }
        Object shared = initializeFrameworkState(availableFeatures,true);
        Class<?> sharedType = shared.getClass();
        Class<?> typeface = Class.forName("android.graphics.Typeface");
        typeface.getMethod("loadPreinstalledSystemFontMap").invoke(null);
        display = new ArtDisplay(apk.applicationInfo.uid);
        sharedType.getMethod("setCurrentAnimatorScale", float.class).invoke(shared, display.animationScale());
        cache.put("display", service("android.hardware.display.IDisplayManager", display::call));
        Class.forName("android.view.ThreadedRenderer").getField("sRendererEnabled").setBoolean(null, false);
        String sessionDescriptor = "android.view.IWindowSession";
        Binder session = service(sessionDescriptor, windows::call);
        display.setWindowSession(session.queryLocalInterface(sessionDescriptor));
        cache.put("window", service("android.view.IWindowManager", display::windowCall));
        ArtInput input = new ArtInput();
        cache.put("input", service("android.hardware.input.IInputManager", (instance, method, arguments) -> {
            switch (method.getName()) {
                case "getVelocityTrackerStrategy": return null;
                case "getInputDevice": return input.device((Integer) arguments[0]);
                case "getInputDeviceIds": return new int[]{android.view.KeyCharacterMap.VIRTUAL_KEYBOARD};
                case "registerInputDevicesChangedListener": return null;
                default: throw new UnsupportedOperationException("ART input service: " + method.getName());
            }
        }));
        int localUser = (Integer) UserHandle.class.getMethod("myUserId").invoke(null);
        ArtInputMethod inputMethod = new ArtInputMethod(localUser);
        cache.put("input_method", service("com.android.internal.view.IInputMethodManager", inputMethod::call));
        String controllerDescriptor = "android.app.IActivityClientController";
        Binder controller = service(controllerDescriptor, activities::call);
        cache.put("activity_task", service("android.app.IActivityTaskManager", (instance, method, arguments) -> {
            switch (method.getName()) {
                case "getActivityClientController": return controller.queryLocalInterface(controllerDescriptor);
                case "getConfiguration": return ArtConfiguration.current();
                case "getDeviceConfigurationInfo": {
                    android.content.res.Configuration current=ArtConfiguration.current();
                    android.content.pm.ConfigurationInfo result=new android.content.pm.ConfigurationInfo();
                    result.reqTouchScreen=current.touchscreen; result.reqKeyboardType=current.keyboard;
                    result.reqNavigation=current.navigation; result.reqGlEsVersion=ArtBoot.glEsVersion();
                    return result;
                }
                case "startActivity": return activities.start((String) arguments[1], resolvedIntent((Intent) arguments[3],(String) arguments[4]),
                    (android.os.IBinder) arguments[5], (String) arguments[6], (Integer) arguments[7]);
                default: throw new UnsupportedOperationException("ART activity task service: " + method.getName());
            }
        }));
        try { ArtDocumentTrees.initialize(apk,localUser); }
        catch(java.io.IOException error) { throw new IllegalStateException("Cannot initialize selected directories",error); }
        cache.put("uri_grants",service("android.app.IUriGrantsManager",ArtDocumentTrees::permissions));
        cache.put("shortcut", service("android.content.pm.IShortcutService", (instance, method, arguments) -> {
            if (method.getName().equals("isRequestPinItemSupported")) {
                if (!apk.applicationInfo.packageName.equals(arguments[0]) || (Integer) arguments[1] != localUser)
                    throw new SecurityException("Shortcut capability queries are restricted to the current application and user");
                // The Windows launcher has no Android shortcut/widget pinning host.
                return false;
            }
            throw new UnsupportedOperationException("ART shortcut service: " + method.getName());
        }));
        ArtMediaSessions.initialize();
        ArtMediaSessions mediaSessions=new ArtMediaSessions(apk,localUser);
        cache.put("media_session",service("android.media.session.ISessionManager",mediaSessions::call));
        storage = new ArtStorage(apk, localUser);
        cache.put("mount", service("android.os.storage.IStorageManager", storage::call));
        ArtClipboard clipboard = new ArtClipboard(apk.applicationInfo.packageName,localUser);
        cache.put("clipboard",service("android.content.IClipboard",clipboard::call));
        cache.put("uimode", service("android.app.IUiModeManager", (instance, method, arguments) -> {
            switch (method.getName()) {
                case "getNightMode": return ArtConfiguration.systemNightMode();
                // Android's contrast listener is independent of the locale/night configuration.
                case "addCallback":
                case "removeCallback": return null;
                case "getContrast": return 0.0f;
                case "getForceInvertState": return 0; // FORCE_INVERT_TYPE_OFF
                case "getForceInvertOverrideState": return 0; // App's own night resources are used.
                case "setApplicationNightMode": ArtConfiguration.setApplicationNightMode((Integer) arguments[0]); return null;
                case "getCurrentModeType": return android.content.res.Configuration.UI_MODE_TYPE_NORMAL;
                case "isUiModeLocked":
                case "isNightModeLocked": return false;
                case "getAttentionModeThemeOverlay": return 1000; // MODE_ATTENTION_THEME_OVERLAY_OFF
                default: throw new UnsupportedOperationException("ART UI mode service: " + method.getName());
            }
        }));
        cache.put("locale", service("android.app.ILocaleManager", (instance, method, arguments) -> {
            if (method.getName().equals("getSystemLocales")) return ArtConfiguration.systemLocales();
            if (arguments == null || arguments.length < 2 || !apk.applicationInfo.packageName.equals(arguments[0])
                    || (Integer) arguments[1] != localUser)
                throw new SecurityException("Locale settings are restricted to the current application");
            switch (method.getName()) {
                case "getApplicationLocales": return ArtConfiguration.applicationLocales();
                case "setApplicationLocales": ArtConfiguration.setApplicationLocales((android.os.LocaleList) arguments[2]); return null;
                case "getOverrideLocaleConfig": return null;
                default: throw new UnsupportedOperationException("ART locale service: " + method.getName());
            }
        }));
        ArtContentObservers content = new ArtContentObservers(localUser);
        broadcasts = new ArtBroadcasts(apk, localUser);
        ArtBattery battery = new ArtBattery(broadcasts);
        ArtWifi wifi = new ArtWifi(apk,broadcasts,hasWifi);
        cache.put("wifi",service("android.net.wifi.IWifiManager",wifi::call));
        power = new ArtPower(apk);
        cache.put("power", service("android.os.IPowerManager", power::call));
        cache.put("deviceidle", service("android.os.IDeviceIdleController", (instance, method, arguments) -> {
            switch (method.getName()) {
                // There is no Android Doze restriction on this Windows process.
                case "isPowerSaveWhitelistApp":
                case "isPowerSaveWhitelistExceptIdleApp": return apk.applicationInfo.packageName.equals(arguments[0]);
                default: throw new UnsupportedOperationException("ART device idle service: " + method.getName());
            }
        }));
        ArtThermal thermal=new ArtThermal();
        cache.put("thermalservice", service("android.os.IThermalService",thermal::call));
        cache.put("batterystats", service("com.android.internal.app.IBatteryStats", battery::stats));
        cache.put("batteryproperties", service("android.os.IBatteryPropertiesRegistrar", battery::properties));
        serviceHost = new ArtServiceHost(apk, localUser);
        pendingIntents = new ArtIntents(apk, localUser, broadcasts, classLoader);
        cache.put("content", service("android.content.IContentService", content::call));
        cache.put("activity", service("android.app.IActivityManager",
            (instance, method, arguments) -> {
                switch (method.getName()) {
                    case "addPackageDependency":
                        if(!webView.noteDependency((String)arguments[0]) && !apk.applicationInfo.packageName.equals(arguments[0]))
                            throw new UnsupportedOperationException("Unknown application package dependency");
                        return null;
                    case "setProcessStateSummary": ArtProcess.stateSummary((byte[])arguments[0]); return null;
                    // Notification permission is explicitly granted through Stone settings.
                    case "getMemoryInfo": {
                        long[] memory = ArtBoot.memoryInfo();
                        if (memory == null || memory.length != 4 || memory[1] == 0)
                            throw new IllegalStateException("Windows memory snapshot is unavailable; restart StoneService");
                        android.app.ActivityManager.MemoryInfo info = (android.app.ActivityManager.MemoryInfo) arguments[0];
                        info.availMem = memory[0]; info.totalMem = memory[1]; info.threshold = memory[2];
                        info.lowMemory = info.availMem < info.threshold;
                        try { info.getClass().getField("advertisedMem").setLong(info,memory[3]); }
                        catch (NoSuchFieldException olderImage) { /* Added in newer framework images. */ }
                        return null;
                    }
                    case "checkPermission":
                    case "checkPermissionForDevice": return (Integer) arguments[2] == apk.applicationInfo.uid
                        && (arguments.length < 4 || (Integer)arguments[3] == android.content.Context.DEVICE_ID_DEFAULT)
                        && (ArtPermissions.networkGranted(apk,(String)arguments[0]) || wifi.granted((String)arguments[0]) || "android.permission.POST_NOTIFICATIONS".equals(arguments[0])
                            && notifications != null && notifications.granted())
                            ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
                    // Log.wtf has already logged the report. There is no system_server
                    // policy to escalate it into an application crash on this host.
                    case "handleApplicationWtf": return false;
                    case "publishContentProviders":
                        // ActivityThread already owns the local providers and authority aliases.
                        // There is no cross-process registry in this single-application host.
                        return null;
                    case "getHistoricalProcessExitReasons": return null;
                    case "getMyMemoryState": {
                        android.app.ActivityManager.RunningAppProcessInfo info = (android.app.ActivityManager.RunningAppProcessInfo)arguments[0];
                        fillProcessInfo(apk,info);
                        return null;
                    }
                    case "getRunningAppProcesses": {
                        android.app.ActivityManager.RunningAppProcessInfo info = new android.app.ActivityManager.RunningAppProcessInfo();
                        fillProcessInfo(apk,info);
                        return Collections.singletonList(info);
                    }
                    case "registerReceiverWithFeature":
                        return broadcasts.register(arguments);
                    case "unregisterReceiver": broadcasts.unregister((IInterface) arguments[0]); return null;
                    case "broadcastIntentWithFeature": return broadcasts.broadcast(arguments);
                    case "getIntentSenderWithFeature":
                        return pendingIntents.create(arguments);
                    case "cancelIntentSender": pendingIntents.cancel((IInterface) arguments[0]); return null;
                    case "sendIntentSender": return pendingIntents.send(arguments);
                    case "getInfoForIntentSender": return pendingIntents.info((IInterface) arguments[0]);
                    case "startService":
                    case "stopService":
                    case "stopServiceToken":
                    case "bindServiceInstance":
                    case "unbindService":
                    case "publishService":
                    case "unbindFinished":
                    case "serviceDoneExecuting":
                    case "setServiceForeground":
                    case "getForegroundServiceType":
                    case "getServices": return serviceHost.call(instance, method, arguments);
                    // RuntimeInit's pre-handler already writes the crash to the
                    // Android log stream. Its terminal handler owns Client exit;
                    // there is no separate Android system crash-dialog process.
                    case "handleApplicationCrash": return null;
                    default: throw new UnsupportedOperationException("ART activity service: " + method.getName());
                }
            }));
        int writeSettingsOp = android.app.AppOpsManager.class.getField("OP_WRITE_SETTINGS").getInt(null);
        int manageStorageOp = android.app.AppOpsManager.class.getField("OP_MANAGE_EXTERNAL_STORAGE").getInt(null);
        cache.put("appops", service("com.android.internal.app.IAppOpsService", (instance, method, arguments) -> {
            if (method.getName().equals("checkOperationRawForDevice")
                    && ((Integer)arguments[0] == writeSettingsOp || (Integer)arguments[0] == manageStorageOp)) {
                if ((Integer)arguments[1] != apk.applicationInfo.uid || !apk.applicationInfo.packageName.equals(arguments[2])
                        || (Integer)arguments[4] != android.content.Context.DEVICE_ID_DEFAULT)
                    return android.app.AppOpsManager.MODE_ERRORED;
                if ((Integer)arguments[0] == manageStorageOp && storage.allFiles()) return android.app.AppOpsManager.MODE_ALLOWED;
                // Windows system settings have no Android write-settings grant.
                return android.app.AppOpsManager.MODE_IGNORED;
            }
            throw new UnsupportedOperationException("ART app ops service: " + method.getName());
        }));
        ArtPermissions permissions = new ArtPermissions();
        String defaultPermissionDevice=(String)Class.forName("android.companion.virtual.VirtualDeviceManager")
            .getField("PERSISTENT_DEVICE_ID_DEFAULT").get(null);
        cache.put("permissionmgr", service("android.permission.IPermissionManager", (instance, method, arguments) -> {
            switch (method.getName()) {
                case "checkPermission":
                    return apk.applicationInfo.packageName.equals(arguments[0]) && (Integer)arguments[3]==localUser
                        && defaultPermissionDevice.equals(arguments[2])
                        && (ArtPermissions.networkGranted(apk,(String)arguments[1]) || wifi.granted((String)arguments[1])
                            || ArtPermissions.NOTIFICATIONS.equals(arguments[1]) && notifications!=null && notifications.granted())
                        ? PackageManager.PERMISSION_GRANTED:PackageManager.PERMISSION_DENIED;
                case "getSplitPermissions": return permissions.splitPermissions((Boolean) arguments[0]);
                case "getPermissionRequestState": return apk.applicationInfo.packageName.equals(arguments[0])
                    && (Integer) arguments[2] == android.content.Context.DEVICE_ID_DEFAULT
                    && ArtPermissions.requestable(apk, (String) arguments[1]) ? (notifications != null && notifications.granted() ? 0
                        : notifications != null && notifications.consented() ? 2 : 1) : 2;
                case "shouldShowRequestPermissionRationale": return false;
                default: throw new UnsupportedOperationException("ART permission service: " + method.getName());
            }
        }));
        cache.put("user", service("android.os.IUserManager", (instance, method, arguments) -> {
            switch (method.getName()) {
                // Stone starts with the current Windows user's application data mounted.
                case "isUserRunning":
                case "isUserUnlocked":
                case "isUserUnlockingOrUnlocked": return (Integer) arguments[0] == localUser;
                case "getProfileType":
                    if ((Integer) arguments[0] != localUser)
                        throw new SecurityException("Profile type is only available for the current Stone user");
                    // IUserManager uses an empty string for a full user. A full
                    // user type name here would incorrectly mark it as a profile.
                    return "";
                case "hasUserRestriction":
                    if ((Integer) arguments[1] != localUser)
                        throw new SecurityException("User restrictions are only available for the current Stone user");
                    // Stone has no Android device/profile owner or managed-user policy.
                    // This query does not grant package-install permission or bypass confirmation.
                    return false;
                case "getApplicationRestrictionsForUser":
                    if((Integer)arguments[1]!=localUser || !apk.applicationInfo.packageName.equals(arguments[0]))
                        throw new SecurityException("Application restrictions are only available for the current application and user");
                    // There is no Android device/profile owner managing this application.
                    return new Bundle();
                default: throw new UnsupportedOperationException("ART user service: " + method.getName());
            }
        }));
        jobs = new ArtJobs(apk);
        cache.put("jobscheduler", service("android.app.job.IJobScheduler", jobs::call));
        ArtConnectivity connectivity = new ArtConnectivity();
        cache.put("connectivity", service("android.net.IConnectivityManager", connectivity::call));
        notifications = new ArtNotifications(apk, localUser);
        cache.put("notification", service("android.app.INotificationManager", notifications::call));
        String installerDescriptor = "android.content.pm.IPackageInstaller";
        packages = new ArtPackageStore(apk);
        uninstaller = new ArtUninstaller(apk, packages);
        ArtPackageInstaller packageInstaller = new ArtPackageInstaller(apk, localUser, packages);
        ArtServices.installer = packageInstaller;
        // Android's shared property service is not running here. Observe atomic
        // registry replacements through the store, including writes by other processes.
        PackageManager.class.getMethod("disablePackageInfoCache").invoke(null);
        PackageManager.class.getMethod("disableApplicationInfoCache").invoke(null);
        Object uidCache = Class.forName("android.app.ApplicationPackageManager").getField("sGetPackagesForUidCache").get(null);
        Class.forName("android.app.PropertyInvalidatedCache").getMethod("disableLocal").invoke(uidCache);
        Binder installer = service(installerDescriptor, packageInstaller::call);
        InstallSourceInfo installSource = InstallSourceInfo.class.getConstructor(
            String.class, SigningInfo.class, String.class, String.class, String.class, int.class)
            .newInstance(null, null, null, null, null, PackageInstaller.PACKAGE_SOURCE_LOCAL_FILE);
        cache.put("package", service("android.content.pm.IPackageManager", (instance, method, arguments) -> {
            String name = method.getName();
            switch (name) {
                case "notifyPackageUse":
                    if(!webView.noteDependency((String)arguments[0]) && !apk.applicationInfo.packageName.equals(arguments[0]))
                        throw new UnsupportedOperationException("Unknown package usage dependency");
                    return null;
                case "hasSystemFeature": return wifi.hasFeature((String)arguments[0],(Integer)arguments[1])
                    || webView.supported() && PackageManager.FEATURE_WEBVIEW.equals(arguments[0]) && (Integer)arguments[1]<=0
                    || hasAudio && PackageManager.FEATURE_AUDIO_OUTPUT.equals(arguments[0]) && (Integer)arguments[1]<=0;
                case "getSystemAvailableFeatures": return method.getReturnType().getConstructor(List.class).newInstance(availableFeatures);
                case "getPackageInstaller": return installer.queryLocalInterface(installerDescriptor);
                case "getPermissionControllerPackageName": return apk.applicationInfo.packageName;
                case "canRequestPackageInstalls": return apk.applicationInfo.packageName.equals(arguments[0])
                    && (Integer) arguments[1] == localUser && packageInstaller.canRequestInstalls();
                case "getInstallerPackageName": {
                    if(webView.packageInfo((String)arguments[0],0)!=null) return null;
                    String[] source = packages.installSource((String) arguments[0]);
                    if (source == null) throw new IllegalArgumentException("Unknown package: " + arguments[0]);
                    return source[0];
                }
                case "getInstallSourceInfo": {
                    if ((Integer) arguments[1] != localUser) return null;
                    if(webView.packageInfo((String)arguments[0],0)!=null)
                        return InstallSourceInfo.class.getConstructor(String.class,SigningInfo.class,String.class,String.class,String.class,int.class)
                            .newInstance(null,null,null,null,null,PackageInstaller.PACKAGE_SOURCE_UNSPECIFIED);
                    String[] source = packages.installSource((String) arguments[0]);
                    if (source == null) return null;
                    String installing = source[0];
                    if (installing == null) return installSource;
                    return InstallSourceInfo.class.getConstructor(String.class, SigningInfo.class, String.class, String.class, String.class, int.class)
                        .newInstance(installing, null, null, installing, source[1], PackageInstaller.PACKAGE_SOURCE_STORE);
                }
                case "getInstalledPackages":
                    return method.getReturnType().getConstructor(List.class).newInstance((Integer) arguments[1] == localUser
                        ? allPackages(((Number) arguments[0]).longValue()) : Collections.emptyList());
                case "isInstantApp": return false;
                case "getApplicationInfo": {
                    android.content.pm.PackageInfo info = (Integer) arguments[2] == localUser
                        ? packageInfo((String) arguments[0], ((Number) arguments[1]).longValue()) : null;
                    return info == null ? null : info.applicationInfo;
                }
                case "getPackageInfo": return (Integer) arguments[2] == localUser
                    ? packageInfo((String) arguments[0], ((Number) arguments[1]).longValue()) : null;
                case "getPackagesForUid": {
                    String[] names = allPackages(0).stream().filter(info -> info.applicationInfo.uid == (Integer) arguments[0])
                        .map(info -> info.packageName).toArray(String[]::new);
                    return names.length == 0 ? null : names;
                }
                case "getPackageUid": {
                    android.content.pm.PackageInfo info = (Integer) arguments[2] == localUser ? packageInfo((String) arguments[0], 0) : null;
                    return info == null ? -1 : info.applicationInfo.uid;
                }
                case "getComponentEnabledSetting": return apk.components.get((ComponentName) arguments[0]);
                case "setComponentEnabledSetting":
                    if (!apk.applicationInfo.packageName.equals(arguments[4]) || (Integer) arguments[3] != localUser)
                        throw new SecurityException("Component state can only be set for the local application and user");
                    apk.components.set((ComponentName) arguments[0], (Integer) arguments[1]);
                    ArtAppearance.changed();
                    return null;
                case "getActivityInfo": {
                    if ((Integer) arguments[2] != localUser) return null;
                    ComponentName component = (ComponentName) arguments[0];
                    ActivityInfo browser = ArtWebLinks.activityInfo(component);
                    if (browser != null) return browser;
                    ActivityInfo share = ArtShares.activityInfo(component);
                    if (share != null) return share;
                    return apk.applicationInfo.packageName.equals(component.getPackageName())
                        ? componentInfo(apk, ActivityInfo.class, arguments)
                        : packages.activityInfo(component, ((Number) arguments[1]).longValue());
                }
                case "resolveIntent": {
                    if ((Integer) arguments[3] != localUser) return null;
                    List<ResolveInfo> matches = queryActivities(apk, resolvedIntent((Intent) arguments[0],(String) arguments[1]), ((Number) arguments[2]).longValue());
                    return matches.isEmpty() ? null : matches.get(0);
                }
                case "queryIntentActivities": {
                    return method.getReturnType().getConstructor(List.class).newInstance(
                        (Integer) arguments[3] == localUser
                            ? queryActivities(apk, resolvedIntent((Intent) arguments[0],(String) arguments[1]), ((Number) arguments[2]).longValue()) : Collections.emptyList());
                }
                case "queryIntentReceivers": return method.getReturnType().getConstructor(List.class).newInstance(
                    (Integer) arguments[3] == localUser
                        ? apk.queryReceivers(resolvedIntent((Intent)arguments[0],(String)arguments[1]),((Number)arguments[2]).longValue())
                        : Collections.emptyList());
                case "queryIntentContentProviders": {
                    Intent intent=(Intent)arguments[0];
                    List<ResolveInfo> matches=new ArrayList<>();
                    if((Integer)arguments[3]==localUser && intent!=null
                        && android.provider.DocumentsContract.PROVIDER_INTERFACE.equals(intent.getAction())
                        && intent.getData()==null && intent.getType()==null && intent.getSelector()==null
                        && (intent.getCategories()==null || intent.getCategories().isEmpty())
                        && (intent.getPackage()==null || "com.trebledot.stone.runtime".equals(intent.getPackage()))) {
                        ProviderInfo info=ArtApplication.hostProviderInfo(apk,ArtDocumentTrees.AUTHORITY);
                        if(intent.getComponent()==null || intent.getComponent().equals(new ComponentName(info.packageName,info.name))) {
                            ResolveInfo match=new ResolveInfo(); match.providerInfo=info; matches.add(match);
                        }
                    }
                    return method.getReturnType().getConstructor(List.class).newInstance(matches);
                }
                case "queryIntentServices": return method.getReturnType().getConstructor(List.class).newInstance(
                    (Integer) arguments[3] == localUser
                        ? apk.queryServices(resolvedIntent((Intent)arguments[0],(String)arguments[1]),((Number)arguments[2]).longValue())
                        : Collections.emptyList());
                case "getServiceInfo": {
                    if((Integer)arguments[2]!=localUser) return null;
                    ComponentName component=(ComponentName)arguments[0];
                    android.content.pm.PackageInfo image=webView.packageInfo(component.getPackageName(),
                        ((Number)arguments[1]).longValue()|PackageManager.GET_SERVICES);
                    if(image!=null && image.services!=null) for(ServiceInfo info:image.services)
                        if(component.getClassName().equals(info.name)) {
                            boolean enabled=image.applicationInfo.enabled && info.enabled;
                            return enabled || (((Number)arguments[1]).longValue()&PackageManager.MATCH_DISABLED_COMPONENTS)!=0?info:null;
                        }
                    return componentInfo(apk, ServiceInfo.class, arguments);
                }
                case "getReceiverInfo": {
                    if ((Integer) arguments[2] != localUser) return null;
                    ComponentName receiverName = (ComponentName) arguments[0];
                    long flags = ((Number) arguments[1]).longValue();
                    android.content.pm.PackageInfo info = apk.applicationInfo.packageName.equals(receiverName.getPackageName()) ? apk.packageInfo
                        : packages.get(receiverName.getPackageName(), PackageManager.GET_RECEIVERS | (flags & PackageManager.GET_META_DATA));
                    if (info == null || info.receivers == null) return null;
                    for (ActivityInfo receiver : info.receivers) if (receiver.name.equals(receiverName.getClassName())) {
                        boolean enabled = info.applicationInfo.enabled && (info == apk.packageInfo ? apk.components.enabled(receiver) : receiver.enabled);
                        if (!enabled && (flags & PackageManager.MATCH_DISABLED_COMPONENTS) == 0) return null;
                        return ArtPackage.receiverCopy(receiver,flags,enabled);
                    }
                    return null;
                }
                case "getProviderInfo": return componentInfo(apk, ProviderInfo.class, arguments);
                case "resolveContentProvider":
                    if((Integer)arguments[2]!=localUser) return null;
                    ProviderInfo host=ArtApplication.hostProviderInfo(apk,(String)arguments[0]);
                    if(host!=null) return host;
                    for (ProviderInfo info : apk.providers) {
                        for (String authority : info.authority.split(";")) if (authority.equals(arguments[0])) return info;
                    }
                    return null;
                default: throw new UnsupportedOperationException("ART package service: " + name);
            }
        }));
    }

    static void initializeWebView(android.app.Application application,ArtPackage apk) throws ReflectiveOperationException {
        webView.initialize(application,apk);
        if(webView.supported()) {
            android.content.pm.FeatureInfo info=new android.content.pm.FeatureInfo(); info.name=PackageManager.FEATURE_WEBVIEW;
            features.add(info);
        }
        Class<?> type=Class.forName("android.content.pm.SystemFeaturesCache");
        Object cache=type.getConstructor(java.util.Collection.class).newInstance(features);
        java.lang.reflect.Field versions=type.getDeclaredField("mSdkFeatureVersions"); versions.setAccessible(true);
        int[] current=(int[])versions.get(frameworkFeatureCache), updated=(int[])versions.get(cache);
        if(current.length!=updated.length) throw new IllegalStateException("System feature cache size changed");
        System.arraycopy(updated,0,current,0,current.length);
        Class<?> sharedType=Class.forName("com.android.internal.os.ApplicationSharedMemory");
        Object shared=sharedType.getMethod("getInstance").invoke(null);
        sharedType.getMethod("writeSystemFeaturesCache",int[].class).invoke(shared,type.getMethod("getSdkFeatureVersions").invoke(cache));
    }

    private static android.content.pm.PackageInfo packageInfo(String name,long flags) throws java.io.IOException {
        android.content.pm.PackageInfo image=webView.packageInfo(name,flags);
        return image==null?packages.get(name,flags):image;
    }

    private static List<android.content.pm.PackageInfo> allPackages(long flags) throws java.io.IOException {
        List<android.content.pm.PackageInfo> result=packages.all(flags);
        if(webView.supported()) {
            result.add(webView.packageInfo(webView.packageName(),flags));
        }
        return result;
    }

    @SuppressWarnings("unchecked")
    static void installRegistry(Map<String, android.os.IBinder> services) throws ReflectiveOperationException {
        Class<?> manager = Class.forName("android.os.ServiceManager");
        Field cacheField = manager.getDeclaredField("sCache");
        cacheField.setAccessible(true);
        Map<String, android.os.IBinder> cache = (Map<String, android.os.IBinder>) cacheField.get(null);
        if (cache != services) { cache.clear(); cache.putAll(services); }
        String registryDescriptor = "android.os.IServiceManager";
        Binder registry = service(registryDescriptor, (instance, method, arguments) -> {
            switch (method.getName()) {
                case "getService":
                case "checkService": return cache.get(arguments[0]);
                case "getService2":
                case "checkService2": {
                    Class<?> metadataType = Class.forName("android.os.ServiceWithMetadata");
                    Object metadata = metadataType.getConstructor().newInstance();
                    metadataType.getField("service").set(metadata, cache.get(arguments[0]));
                    return method.getReturnType().getMethod("serviceWithMetadata", metadataType).invoke(null, metadata);
                }
                case "listServices": return cache.keySet().stream().sorted().toArray(String[]::new);
                case "isDeclared": return cache.containsKey(arguments[0]);
                case "getDeclaredInstances": return new String[0];
                default: throw new UnsupportedOperationException("ART service registry: " + method.getName());
            }
        });
        Field registryField = manager.getDeclaredField("sServiceManager");
        registryField.setAccessible(true);
        registryField.set(null, registry.queryLocalInterface(registryDescriptor));
    }

    private static <T extends ComponentInfo> T componentInfo(ArtPackage apk, Class<T> type, Object[] arguments) {
        if (type == ActivityInfo.class) {
            ActivityInfo permission = ArtPermissions.info(apk, (ComponentName) arguments[0]);
            if (permission != null) return type.cast(permission);
            ActivityInfo locale = ArtLocaleSettings.info(apk,(ComponentName) arguments[0]);
            if (locale != null) return type.cast(locale);
            ActivityInfo storageAccess = ArtStorageAccess.info(apk,(ComponentName) arguments[0]);
            if (storageAccess != null) return type.cast(storageAccess);
            ActivityInfo document = ArtDocuments.info(apk,(ComponentName) arguments[0]);
            if (document != null) return type.cast(document);
        }
        if (type == ActivityInfo.class && installer != null) {
            ActivityInfo confirmation = installer.confirmationInfo((ComponentName) arguments[0]);
            if (confirmation != null) return type.cast(confirmation);
        }
        if (type == ActivityInfo.class && uninstaller != null) {
            ActivityInfo removal = uninstaller.info((ComponentName) arguments[0]);
            if (removal != null) return type.cast(removal);
        }
        ComponentInfo info = apk.components.find((ComponentName) arguments[0]);
        if (!type.isInstance(info) || (((Number) arguments[1]).longValue() & PackageManager.MATCH_DISABLED_COMPONENTS) == 0
                && !apk.components.enabled(info)) return null;
        return type.cast(info);
    }

    private static Intent resolvedIntent(Intent intent, String type) {
        return intent == null || type == null || type.equals(intent.getType()) ? intent : new Intent(intent).setDataAndType(intent.getData(),type);
    }

    private static List<ResolveInfo> queryActivities(ArtPackage apk, Intent intent, long flags) throws java.io.IOException {
        List<ResolveInfo> result = new java.util.ArrayList<>();
        ActivityInfo target = ArtPermissions.resolve(apk, intent);
        if (target == null) target = ArtLocaleSettings.resolve(apk,intent);
        if (target == null) target = ArtStorageAccess.resolve(apk,intent);
        if (target == null) target = ArtDocuments.resolve(apk,intent);
        if (target == null && installer != null) target = installer.resolveConfirmation(intent);
        if (target == null && uninstaller != null) target = uninstaller.resolve(intent);
        if (target != null) {
            ResolveInfo resolved = new ResolveInfo(); resolved.activityInfo = target;
            result.add(resolved);
        }
        if (target == null) result.addAll(apk.queryActivities(intent,flags));
        for (ResolveInfo match : packages.queryActivities(intent, flags)) {
            if (!apk.applicationInfo.packageName.equals(match.activityInfo.packageName)) result.add(match);
        }
        ActivityInfo browser = ArtWebLinks.resolve(intent);
        if (browser != null) { ResolveInfo resolved = new ResolveInfo(); resolved.activityInfo = browser; result.add(resolved); }
        ActivityInfo share = ArtShares.resolve(intent);
        if (share != null) { ResolveInfo resolved = new ResolveInfo(); resolved.activityInfo = share; result.add(resolved); }
        return result;
    }

    static Object initializeFrameworkState() throws ReflectiveOperationException {
        return initializeFrameworkState(Collections.emptyList(),false);
    }

    private static Object initializeFrameworkState(java.util.Collection<android.content.pm.FeatureInfo> available,boolean deferFeatures) throws ReflectiveOperationException {
        Class<?> sharedType = Class.forName("com.android.internal.os.ApplicationSharedMemory");
        Object shared = sharedType.getMethod("create").invoke(null);
        sharedType.getMethod("setInstance", sharedType).invoke(null, shared);
        Class<?> features = Class.forName("android.content.pm.SystemFeaturesCache");
        Object featureCache = features.getConstructor(java.util.Collection.class).newInstance(available);
        features.getMethod("setInstance", features).invoke(null, featureCache);
        frameworkFeatureCache=featureCache;
        if(!deferFeatures) sharedType.getMethod("writeSystemFeaturesCache", int[].class).invoke(shared,
            features.getMethod("getSdkFeatureVersions").invoke(featureCache));
        return shared;
    }

    private static void fillProcessInfo(ArtPackage apk,android.app.ActivityManager.RunningAppProcessInfo info) {
        info.pid=android.os.Process.myPid(); info.uid=apk.applicationInfo.uid;
        info.processName=ArtProcess.current(apk); info.pkgList=new String[]{apk.applicationInfo.packageName};
        info.importance=Math.min(activities.importance(),serviceHost.importance());
        info.lastTrimLevel=0; info.lru=0; info.importanceReasonCode=0;
        info.importanceReasonPid=0; info.importanceReasonComponent=null;
    }

    static Binder service(String descriptor, InvocationHandler handler) throws ReflectiveOperationException {
        Binder binder = new Binder();
        Class<?> type = Class.forName(descriptor);
        IInterface service = (IInterface) Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[]{type},
            (instance, method, arguments) -> {
                switch (method.getName()) {
                    case "asBinder": return binder;
                    case "toString": return "Stone local " + descriptor;
                    case "hashCode": return System.identityHashCode(instance);
                    case "equals": return instance == arguments[0];
                    default:
                        try { return handler.invoke(instance, method, arguments); }
                        catch(Throwable error) {
                            if(Boolean.getBoolean("stone.diagnostics"))
                                android.util.Log.e("StoneBinder",descriptor+"."+method.getName(),error);
                            throw error;
                        }
                }
            });
        binder.attachInterface(service, descriptor);
        return binder;
    }

}
