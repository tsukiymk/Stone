package com.trebledot.stone.runtime;

import android.app.Application;
import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ProviderInfo;
import android.os.Handler;
import android.os.IBinder;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.lang.ref.WeakReference;

/** Connects an APK to the selected image's application framework, without a zygote. */
public final class ArtApplication {
    public final ArtPackage apk;
    public final Context context;
    public final Application application;
    private Application systemApplication;
    private ArtPackageEvents packageEvents;
    private final Object thread;
    private final Object loadedApk;
    private final Handler main = new Handler(android.os.Looper.getMainLooper());
    private final ArrayList<Screen> task = new ArrayList<>();
    private enum Phase { ATTACHED, CREATED, STARTED, RESUMED, PAUSED, STOPPED, DESTROYED }
    private volatile int activityImportance = android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED;

    int activityImportance() { return activityImportance; }

    private static final class Screen {
        final Activity activity;
        final IBinder token;
        final IBinder caller;
        final String resultWho;
        final int requestCode;
        final ActivityInfo info;
        Object client;
        Phase phase = Phase.ATTACHED;

        Screen(Activity activity, IBinder token, IBinder caller, String resultWho, int requestCode, ActivityInfo info) {
            this.activity = activity;
            this.token = token;
            this.caller = caller;
            this.resultWho = resultWho;
            this.requestCode = requestCode;
            this.info = info;
        }
    }

    private ArtApplication(ArtPackage apk, Context context, Application application,
            Object thread, Object loadedApk) {
        this.apk = apk;
        this.context = context;
        this.application = application;
        this.thread = thread;
        this.loadedApk = loadedApk;
    }

    private Screen createActivity(ActivityInfo info, Intent intent, IBinder caller, String who, int request)
            throws ReflectiveOperationException {
        return createActivity(info, intent, caller, who, request, null, null);
    }

    private Screen createActivity(ActivityInfo info, Intent intent, IBinder caller, String who, int request,
            IBinder existingToken, Object retained) throws ReflectiveOperationException {
        Instrumentation instrumentation = (Instrumentation) field(thread.getClass(), "mInstrumentation").get(thread);
        String name = info.targetActivity == null ? info.name : info.targetActivity;
        Activity instance = instrumentation.newActivity(context.getClassLoader(), name, intent);
        if (apk.applicationInfo.metaData != null && apk.applicationInfo.metaData.getInt("flutterEmbedding",0) == 2) {
            // The Windows Surface queue uses the existing ANGLE/OpenGL readback
            // path. Android Impeller's Vulkan surface integration is unavailable.
            intent.putExtra("enable-impeller",false);
        }
        IBinder token = existingToken == null ? ArtActivityToken.create() : existingToken;
        ArtServices.activities.register(token, info, caller, request);
        try {
            attachActivity(instance, token, info, intent, instrumentation, retained);
            Screen screen = new Screen(instance, token, caller, who, request, info);
            registerClient(screen, intent);
            return screen;
        } catch (ReflectiveOperationException | RuntimeException | Error error) {
            ArtServices.activities.unregister(token,false);
            throw error;
        }
    }

    @SuppressWarnings("unchecked")
    private Map<IBinder, Object> clients() throws ReflectiveOperationException {
        return (Map<IBinder, Object>) field(thread.getClass(), "mActivities").get(thread);
    }

    private void registerClient(Screen screen, Intent intent) throws ReflectiveOperationException {
        Class<?> type = Class.forName("android.app.ActivityThread$ActivityClientRecord");
        Object client = construct(type, new Class<?>[0]);
        field(type, "token").set(client, screen.token);
        field(type, "activity").set(client, screen.activity);
        field(type, "activityInfo").set(client, screen.info);
        field(type, "intent").set(client, intent);
        field(type, "window").set(client, screen.activity.getWindow());
        field(type, "packageInfo").set(client, loadedApk);
        field(type, "createdConfig").set(client, new android.content.res.Configuration(screen.activity.getResources().getConfiguration()));
        field(type, "overrideConfig").set(client, new android.content.res.Configuration());
        screen.client = client;
        clients().put(screen.token, client);
    }

    private void phase(Screen screen, Phase phase) throws ReflectiveOperationException {
        screen.phase = phase;
        invoke(screen.client.getClass(), screen.client, "setState", new Class<?>[]{int.class}, phase.ordinal());
        int importance = android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED;
        for (Screen current : task) {
            if (current.phase == Phase.RESUMED) importance = Math.min(importance, android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND);
            else if (current.phase == Phase.STARTED || current.phase == Phase.PAUSED)
                importance = Math.min(importance, android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_VISIBLE);
        }
        activityImportance = importance;
    }

    private void installRelaunchHandler() throws ReflectiveOperationException {
        Handler handler = (Handler) invoke(thread.getClass(), thread, "getHandler", new Class<?>[0]);
        Field callback = field(Handler.class, "mCallback");
        Handler.Callback previous = (Handler.Callback) callback.get(handler);
        int relaunch = field(Class.forName("android.app.ActivityThread$H"), "RELAUNCH_ACTIVITY").getInt(null);
        callback.set(handler, (Handler.Callback) message -> {
            if (message.what == relaunch) {
                for (int index = 0; index < task.size(); index++) if (task.get(index).token == message.obj) {
                    try { recreate(index, 0); }
                    catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not recreate ART Activity", error); }
                    return true;
                }
            }
            return previous != null && previous.handleMessage(message);
        });
    }

    private void attachActivity(Activity instance, IBinder token, ActivityInfo info, Intent intent, Instrumentation instrumentation, Object retained)
            throws ReflectiveOperationException {
        Class<?> contextType = Class.forName("android.app.ContextImpl");
        Context base = (Context) invoke(contextType, null, "createActivityContext",
            new Class<?>[]{thread.getClass(), loadedApk.getClass(), android.content.pm.ActivityInfo.class,
                android.os.IBinder.class, int.class, android.content.res.Configuration.class},
            thread, loadedApk, info, token, 0, new android.content.res.Configuration());
        invoke(contextType, base, "setOuterContext", new Class<?>[]{Context.class}, instance);
        Application owner=application;
        if (instance.getClass().getName().startsWith("com.trebledot.stone.runtime.")) {
            // Host settings/document/consent pages stand in for another Android
            // process. APK lifecycle callbacks must only observe APK Activities.
            if (systemApplication==null) {
                systemApplication=new Application();
                invoke(Application.class,systemApplication,"attach",new Class<?>[]{Context.class},context);
            }
            owner=systemApplication;
        }
        Object[] arguments = {base, thread, instrumentation, token, 1, owner, intent,
            info, info.loadLabel(context.getPackageManager()), null, null, retained,
            new android.content.res.Configuration(base.getResources().getConfiguration()), null, null, null,
            null, new android.os.Binder(), new android.os.Binder(), null};
        Method attach = null;
        for (Method candidate : Activity.class.getDeclaredMethods()) {
            if (candidate.getName().equals("attach") && candidate.getParameterTypes().length == arguments.length) {
                attach = candidate;
                break;
            }
        }
        if (attach == null) throw new NoSuchMethodException("Unsupported image Activity.attach signature");
        attach.setAccessible(true);
        try { attach.invoke(instance, arguments); }
        catch (InvocationTargetException error) { throw cause(error); }
        int theme = info.getThemeResource();
        if (theme != 0) instance.setTheme(theme);
    }

    /** Runs the framework's create, start and resume lifecycle and Application callbacks. */
    public Activity startActivity() throws ReflectiveOperationException {
        requireMainThread();
        if (!task.isEmpty()) throw new IllegalStateException("The launcher Activity already exists");
        Intent intent = apk.launchIntent();
        return launch(apk.resolveActivity(intent), intent, null, null, -1).activity;
    }

    private Screen launch(ActivityInfo info, Intent intent, IBinder caller, String who, int request)
            throws ReflectiveOperationException {
        return launch(info,intent,caller,who,request,null,null);
    }
    void launchExternal(ActivityInfo info, Intent intent, String route, android.content.ComponentName caller)
            throws ReflectiveOperationException {
        launch(info,intent,null,null,-1,route,caller);
    }
    private Screen launch(ActivityInfo info, Intent intent, IBinder caller, String who, int request,
            String route, android.content.ComponentName remoteCaller) throws ReflectiveOperationException {
        if (Boolean.getBoolean("stone.diagnostics")) android.util.Log.i("StoneActivity","Launch " + info.name);
        Screen previous = top();
        Screen next = createActivity(info, intent, caller, who, request);
        if (route != null) ArtServices.activities.attachExternal(next.token,route,remoteCaller);
        try {
            if (previous != null) pause(previous);
            task.add(next);
            invoke(Activity.class, next.activity, "performCreate", new Class<?>[]{android.os.Bundle.class}, (Object) null);
            phase(next, Phase.CREATED);
            if (!next.activity.isFinishing()) {
                invoke(Activity.class, next.activity, "performStart", new Class<?>[]{String.class}, "Stone");
                phase(next, Phase.STARTED);
            }
            resume(next);
            if (previous != null && !next.activity.isFinishing()) stop(previous);
            return next;
        } catch (ReflectiveOperationException | RuntimeException | Error failure) {
            task.remove(next);
            try { destroy(next); }
            catch (ReflectiveOperationException | RuntimeException | Error cleanup) { failure.addSuppressed(cleanup); }
            throw failure;
        }
    }

    private Screen top() {
        return task.isEmpty() ? null : task.get(task.size() - 1);
    }

    Activity currentActivity(Activity fallback) {
        Screen current = top();
        return current == null ? fallback : current.activity;
    }
    Intent retainedIntent(Activity activity) throws ReflectiveOperationException {
        Intent current = activity.getIntent(); if (current != null) return new Intent(current);
        for (Screen screen : task) if (screen.activity == activity) {
            Intent request = (Intent) field(screen.client.getClass(),"intent").get(screen.client);
            return request == null ? null : new Intent(request);
        }
        return null;
    }

    boolean isTop(IBinder token) {
        Screen current = top();
        return current != null && current.token == token;
    }

    boolean hasPreviousActivity() { return task.size() > 1; }

    void scheduleService(String name, Class<?>[] signature, Object... args) throws ReflectiveOperationException {
        Object app = invoke(thread.getClass(), thread, "getApplicationThread", new Class<?>[0]);
        invoke(app.getClass(), app, name, signature, args);
    }

    void scheduleReceiver(ActivityInfo info, Intent intent, int uid, String sender) throws ReflectiveOperationException {
        Object app = invoke(thread.getClass(), thread, "getApplicationThread", new Class<?>[0]);
        Class<?> compatibility = Class.forName("android.content.res.CompatibilityInfo");
        invoke(app.getClass(), app, "scheduleReceiver", new Class<?>[]{Intent.class, ActivityInfo.class, compatibility,
                int.class, String.class, android.os.Bundle.class, boolean.class, boolean.class, int.class, int.class, int.class, String.class},
            new Intent(intent), info, field(compatibility, "DEFAULT_COMPATIBILITY_INFO").get(null), 0, null, null,
            false, true, (Integer) android.os.UserHandle.class.getMethod("myUserId").invoke(null), 2, uid, sender);
    }

    void scheduleRegisteredReceiver(android.os.IInterface receiver, Intent intent, int uid, String sender) throws ReflectiveOperationException {
        scheduleRegisteredReceiver(receiver, intent, uid, sender, false);
    }

    void scheduleRegisteredReceiver(android.os.IInterface receiver, Intent intent, int uid, String sender, boolean initialSticky) throws ReflectiveOperationException {
        Object app = invoke(thread.getClass(), thread, "getApplicationThread", new Class<?>[0]);
        invoke(app.getClass(), app, "scheduleRegisteredReceiver", new Class<?>[]{Class.forName("android.content.IIntentReceiver"), Intent.class,
                int.class, String.class, android.os.Bundle.class, boolean.class, boolean.class, boolean.class, int.class, int.class, int.class, String.class},
            receiver, new Intent(intent), 0, null, null, false, initialSticky, true,
            (Integer) android.os.UserHandle.class.getMethod("myUserId").invoke(null), 2, uid, sender);
    }

    void postConfiguration(android.content.res.Configuration config, int changes) {
        final int relevant = changes & (ActivityInfo.CONFIG_LOCALE | ActivityInfo.CONFIG_LAYOUT_DIRECTION | ActivityInfo.CONFIG_UI_MODE
            | ActivityInfo.CONFIG_SCREEN_SIZE | ActivityInfo.CONFIG_SMALLEST_SCREEN_SIZE | ActivityInfo.CONFIG_ORIENTATION | ActivityInfo.CONFIG_SCREEN_LAYOUT);
        if (relevant == 0) return;
        main.post(() -> {
            try {
                Object controller = field(thread.getClass(), "mConfigurationController").get(thread);
                if (controller != null) invoke(controller.getClass(), controller, "setConfiguration",
                    new Class<?>[]{android.content.res.Configuration.class}, config);
                application.onConfigurationChanged(new android.content.res.Configuration(config));
                for (int index = 0; index < task.size(); index++) {
                    Screen previous = task.get(index);
                    if ((previous.info.configChanges & relevant) == relevant) {
                        android.content.res.Configuration local = new android.content.res.Configuration(previous.activity.getResources().getConfiguration());
                        field(Activity.class, "mConfigChangeFlags").setInt(previous.activity, 0);
                        field(Activity.class, "mCurrentConfig").set(previous.activity, new android.content.res.Configuration(local));
                        field(Activity.class, "mCalled").setBoolean(previous.activity, false);
                        previous.activity.onConfigurationChanged(local);
                        if (!field(Activity.class, "mCalled").getBoolean(previous.activity))
                            throw new IllegalStateException("Activity must call super.onConfigurationChanged: " + previous.info.name);
                        previous.activity.getWindow().getDecorView().dispatchConfigurationChanged(local);
                        continue;
                    }
                    recreate(index, relevant);
                }
                ArtAppearance.changed();
            } catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not update ART activities for Windows configuration", error); }
        });
    }

    private void recreate(int index, int changes) throws ReflectiveOperationException {
        Screen previous = task.get(index);
        if (previous.activity.isFinishing()) return;
        boolean visible = previous == top();
        // An APK may clear its Intent during onDestroy. Snapshot before teardown
        // and fall back to the retained launch request if it was already cleared.
        Intent request = previous.activity.getIntent();
        if (request == null) request = (Intent) field(previous.client.getClass(), "intent").get(previous.client);
        Intent intent = new Intent(request);
        if (intent.getComponent() == null) intent.setComponent(new android.content.ComponentName(previous.info.packageName, previous.info.name));
        intent.setExtrasClassLoader(context.getClassLoader());
        Instrumentation instrumentation = (Instrumentation) field(thread.getClass(), "mInstrumentation").get(thread);
        pause(previous);
        android.os.Bundle saved = new android.os.Bundle();
        saved.setClassLoader(context.getClassLoader());
        instrumentation.callActivityOnSaveInstanceState(previous.activity, saved);
        field(Activity.class, "mConfigChangeFlags").setInt(previous.activity, changes);
        field(Activity.class, "mChangingConfigurations").setBoolean(previous.activity, true);
        Object retained = invoke(Activity.class, previous.activity, "retainNonConfigurationInstances", new Class<?>[0]);
        destroy(previous,true);
        Screen next = createActivity(previous.info, intent, previous.caller,
            previous.resultWho, previous.requestCode, previous.token, retained);
        task.set(index, next);
        invoke(Activity.class, next.activity, "performCreate", new Class<?>[]{android.os.Bundle.class}, saved);
        phase(next, Phase.CREATED);
        if (next.activity.isFinishing()) return;
        invoke(Activity.class, next.activity, "performStart", new Class<?>[]{String.class}, "Stone recreation");
        phase(next, Phase.STARTED);
        if (next.activity.isFinishing()) return;
        instrumentation.callActivityOnRestoreInstanceState(next.activity, saved);
        if (visible) resume(next); else stop(next);
    }

    boolean postLaunch(ActivityInfo info, Intent request, IBinder caller, String who, int requestCode) {
        Intent intent = new Intent(request).setComponent(new android.content.ComponentName(info.packageName, info.name));
        return main.post(() -> {
            try { launch(info, intent, caller, who, requestCode); }
            catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not launch ART Activity " + info.name, error); }
        });
    }

    boolean postFinish(IBinder token, int resultCode, Intent result) {
        Intent data = result == null ? null : new Intent(result);
        return main.post(() -> {
            try { finish(token, resultCode, data); }
            catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not finish ART Activity", error); }
        });
    }

    private void finish(IBinder token, int resultCode, Intent result) throws ReflectiveOperationException {
        for (int i = task.size() - 1; i >= 0; i--) {
            Screen screen = task.get(i);
            if (screen.token != token) continue;
            if (Boolean.getBoolean("stone.diagnostics")) android.util.Log.i("StoneActivity","Finish " + screen.info.name + " result=" + resultCode);
            boolean wasTop = screen == top();
            ArtServices.activities.finishExternal(token,resultCode,result);
            task.remove(i);
            destroy(screen);
            Screen next = top();
            if (next == null) {
                ArtWindow.close();
            } else {
                if (wasTop && next.phase == Phase.STOPPED) {
                    invoke(Activity.class, next.activity, "performRestart", new Class<?>[]{boolean.class}, true);
                    phase(next, Phase.STARTED);
                }
                if (screen.requestCode >= 0) {
                    for (Screen target : task) {
                        if (target.token == screen.caller) {
                            invoke(Activity.class, target.activity, "dispatchActivityResult",
                                new Class<?>[]{String.class, int.class, int.class, Intent.class, String.class},
                                screen.resultWho, screen.requestCode, resultCode, result, "Stone");
                            break;
                        }
                    }
                }
                if (wasTop) resume(next);
            }
            return;
        }
    }

    private void pause(Screen screen) throws ReflectiveOperationException {
        if (screen.phase != Phase.RESUMED) return;
        invoke(Activity.class, screen.activity, "performPause", new Class<?>[0]);
        phase(screen, Phase.PAUSED);
    }

    private void stop(Screen screen) throws ReflectiveOperationException {
        if (screen.phase != Phase.STARTED && screen.phase != Phase.PAUSED) return;
        invoke(Activity.class, screen.activity, "performStop", new Class<?>[]{boolean.class, String.class}, false, "Stone");
        phase(screen, Phase.STOPPED);
    }

    private void resume(Screen screen) throws ReflectiveOperationException {
        if (screen.activity.isFinishing()) return;
        invoke(Activity.class, screen.activity, "performResume", new Class<?>[]{boolean.class, String.class}, false, "Stone");
        phase(screen, Phase.RESUMED);
    }

    private void destroy(Screen screen) throws ReflectiveOperationException {
        destroy(screen,false);
    }
    private void destroy(Screen screen, boolean recreating) throws ReflectiveOperationException {
        if (screen.phase == Phase.DESTROYED) return;
        ArtServices.windows.stopActivitySurfaceProducers(screen.activity);
        try {
            pause(screen);
            stop(screen);
            if (screen.phase != Phase.ATTACHED)
                invoke(Activity.class, screen.activity, "performDestroy", new Class<?>[0]);
        } finally {
            try { ArtServices.windows.closeActivity(screen.activity); }
            finally {
                phase(screen, Phase.DESTROYED);
                clients().remove(screen.token, screen.client);
                ArtServices.activities.unregister(screen.token,recreating);
            }
        }
    }

    void deliverExternalResult(IBinder caller, String who, int request, int result, Intent data) throws ReflectiveOperationException {
        requireMainThread();
        for (Screen screen : task) if (screen.token == caller) {
            invoke(Activity.class,screen.activity,"dispatchActivityResult",
                new Class<?>[]{String.class,int.class,int.class,Intent.class,String.class},who,request,result,data,"Stone");
            return;
        }
    }

    /** Stops the current Activity before the host releases its package resources. */
    public void destroyActivity() throws ReflectiveOperationException {
        requireMainThread();
        while (!task.isEmpty()) destroy(task.remove(task.size() - 1));
    }

    private static void requireMainThread() {
        if (android.os.Looper.myLooper() != android.os.Looper.getMainLooper())
            throw new IllegalStateException("Activity lifecycle requires the main Looper thread");
    }

    /** Called once on the prepared ART main thread. The APK must already be on its class path. */
    @SuppressWarnings("unchecked")
    public static ArtApplication start(ArtPackage apk) throws ReflectiveOperationException {
        return start(apk, ClassLoader.getSystemClassLoader());
    }

    @SuppressWarnings("unchecked")
    static ArtApplication start(ArtPackage apk, ClassLoader classLoader) throws ReflectiveOperationException {
        Class<?> threadType = Class.forName("android.app.ActivityThread");
        if (field(threadType, "sCurrentActivityThread").get(null) != null)
            throw new IllegalStateException("An ART application is already running");
        Class<?> loadedType = Class.forName("android.app.LoadedApk");
        Class<?> compatibilityType = Class.forName("android.content.res.CompatibilityInfo");
        Class<?> contextType = Class.forName("android.app.ContextImpl");
        if (android.os.Looper.myLooper() == null || android.os.Looper.myLooper() != android.os.Looper.getMainLooper())
            throw new IllegalStateException("ART application startup requires the main Looper thread");
        ArtRuntimeInit.initialize();
        ArtPackageEvents packageEvents = null;
        ArtServices.install(apk, classLoader);
        ArtConfiguration.attach(apk);
        field(android.content.pm.ApplicationInfo.class, "credentialProtectedDataDir")
            .set(apk.applicationInfo, apk.applicationInfo.dataDir);
        Object thread = construct(threadType, new Class<?>[0]);
        Instrumentation instrumentation = new Instrumentation();
        field(threadType, "mInstrumentation").set(thread, instrumentation);
        field(threadType, "sMainThreadHandler").set(null, invoke(threadType, thread, "getHandler", new Class<?>[0]));
        field(threadType, "sCurrentActivityThread").set(null, thread);
        try {
            Object loaded = construct(loadedType, new Class<?>[]{threadType,
                android.content.pm.ApplicationInfo.class, compatibilityType, ClassLoader.class,
                boolean.class, boolean.class, boolean.class}, thread, apk.applicationInfo,
                field(compatibilityType, "DEFAULT_COMPATIBILITY_INFO").get(null), classLoader,
                false, true, false);
            // Explicit APKs use the loader supplied at VM creation; package-name
            // launches use a PathClassLoader after resolving the durable index.
            field(loadedType, "mClassLoader").set(loaded, classLoader);
            field(loadedType, "mDefaultClassLoader").set(loaded, classLoader);
            field(loadedType, "mResources").set(loaded, apk.resources);
            ((Map<String, WeakReference<Object>>) field(threadType, "mPackages").get(thread))
                .put(apk.applicationInfo.packageName, new WeakReference<>(loaded));
            Class<?> boundType = Class.forName("android.app.ActivityThread$AppBindData");
            Object bound = construct(boundType, new Class<?>[0]);
            field(boundType, "info").set(bound, loaded);
            field(boundType, "appInfo").set(bound, apk.applicationInfo);
            field(boundType, "processName").set(bound, ArtProcess.current(apk));
            field(threadType, "mBoundApplication").set(thread, bound);
            Context context = (Context) invoke(contextType, null, "createAppContext",
                new Class<?>[]{threadType, loadedType}, thread, loaded);
            invoke(Instrumentation.class, instrumentation, "init", new Class<?>[]{threadType,
                Context.class, Context.class, android.content.ComponentName.class,
                Class.forName("android.app.IInstrumentationWatcher"), Class.forName("android.app.IUiAutomationConnection")},
                thread, context, context, null, null, null);
            installSettings(thread, context, apk);
            try { ArtServices.packages.initializeLaunchers(); }
            catch (java.io.IOException error) { throw new IllegalStateException("Cannot prepare installed application launchers", error); }
            String name = apk.applicationInfo.className;
            Thread.currentThread().setContextClassLoader(classLoader);
            Application application = instrumentation.newApplication(classLoader,
                name == null ? "android.app.Application" : name, context);
            field(loadedType, "mApplication").set(loaded, application);
            field(threadType, "mInitialApplication").set(thread, application);
            ((List<Application>) field(threadType, "mAllApplications").get(thread)).add(application);
            invoke(contextType, context, "setOuterContext", new Class<?>[]{Context.class}, application);
            List<ProviderInfo> providers = apk.localProviders();
            if (!providers.isEmpty()) invoke(threadType, thread, "installContentProviders",
                new Class<?>[]{Context.class, List.class}, application, providers);
            ArtApplication running = new ArtApplication(apk, context, application, thread, loaded);
            try { packageEvents = new ArtPackageEvents(ArtServices.packages, ArtServices.broadcasts, apk.applicationInfo.packageName); }
            catch (java.io.IOException error) { throw new IllegalStateException("Cannot observe installed package changes", error); }
            running.packageEvents = packageEvents;
            ArtServices.activities.setApplication(running);
            running.installRelaunchHandler();
            packageEvents.start();
            instrumentation.callApplicationOnCreate(application);
            ArtServices.pendingIntents.attach();
            return running;
        } catch (ReflectiveOperationException | RuntimeException | Error failure) {
            if (packageEvents != null) packageEvents.close();
            ArtServices.activities.setApplication(null);
            field(threadType, "sCurrentActivityThread").set(null, null);
            field(threadType, "sMainThreadHandler").set(null, null);
            throw failure;
        }
    }

    private static void installSettings(Object thread, Context context, ArtPackage apk) throws ReflectiveOperationException {
        installHostProvider(thread,context,apk,"settings",new ArtSettingsProvider(),true);
        installHostProvider(thread,context,apk,ArtDocuments.AUTHORITY,new ArtDocumentProvider(),false);
        installHostProvider(thread,context,apk,ArtDocumentTrees.AUTHORITY,new ArtTreeProvider(),true);
    }
    private static void installHostProvider(Object thread, Context context, ArtPackage apk, String authority,
            android.content.ContentProvider provider, boolean exported) throws ReflectiveOperationException {
        for (android.content.pm.ProviderInfo candidate : apk.providers) {
            for (String name : candidate.authority.split(";")) {
                if (name.equals(authority)) throw new IllegalArgumentException("APK declares a reserved host authority: " + authority);
            }
        }
        android.content.pm.ProviderInfo info = hostProviderInfo(apk,authority);
        if(info==null) throw new IllegalArgumentException("Unknown host provider");
        provider.attachInfo(context, info);
        Class<?> holderType = Class.forName("android.app.ContentProviderHolder");
        Object holder = construct(holderType, new Class<?>[]{android.content.pm.ProviderInfo.class}, info);
        field(holderType, "provider").set(holder, invoke(android.content.ContentProvider.class,
            provider, "getIContentProvider", new Class<?>[0]));
        field(holderType, "noReleaseNeeded").setBoolean(holder, true);
        invoke(thread.getClass(), thread, "installProvider", new Class<?>[]{Context.class, holderType,
            android.content.pm.ProviderInfo.class, boolean.class, boolean.class, boolean.class},
            context, holder, info, false, true, true);
    }
    static android.content.pm.ProviderInfo hostProviderInfo(ArtPackage apk,String authority) {
        Class<?> type=ArtDocumentTrees.AUTHORITY.equals(authority)?ArtTreeProvider.class
            : ArtDocuments.AUTHORITY.equals(authority)?ArtDocumentProvider.class
            : "settings".equals(authority)?ArtSettingsProvider.class:null;
        if(type==null) return null;
        android.content.pm.ProviderInfo info = new android.content.pm.ProviderInfo();
        info.name = type.getName();
        info.packageName = "com.trebledot.stone.runtime";
        info.authority = authority;
        info.enabled = true;
        info.exported = type!=ArtDocumentProvider.class;
        if(type==ArtTreeProvider.class) {
            info.grantUriPermissions=true;
            info.readPermission=info.writePermission="android.permission.MANAGE_DOCUMENTS";
        }
        info.applicationInfo = new android.content.pm.ApplicationInfo(apk.applicationInfo);
        info.applicationInfo.packageName = info.packageName;
        return info;
    }

    private static Field field(Class<?> type, String name) throws ReflectiveOperationException {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static Object construct(Class<?> type, Class<?>[] types, Object... arguments)
            throws ReflectiveOperationException {
        Constructor<?> constructor = type.getDeclaredConstructor(types);
        constructor.setAccessible(true);
        try { return constructor.newInstance(arguments); }
        catch (InvocationTargetException error) { throw cause(error); }
    }

    private static Object invoke(Class<?> type, Object instance, String name, Class<?>[] types,
            Object... arguments) throws ReflectiveOperationException {
        Method method = type.getDeclaredMethod(name, types);
        method.setAccessible(true);
        try { return method.invoke(instance, arguments); }
        catch (InvocationTargetException error) { throw cause(error); }
    }

    private static ReflectiveOperationException cause(InvocationTargetException error) {
        Throwable cause = error.getCause();
        if (cause instanceof RuntimeException) throw (RuntimeException) cause;
        if (cause instanceof Error) throw (Error) cause;
        return cause instanceof ReflectiveOperationException ? (ReflectiveOperationException) cause : error;
    }
}
