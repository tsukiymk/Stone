package com.trebledot.stone.probe;

import android.app.Activity;
import android.app.LocaleManager;
import android.app.UiModeManager;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.LocaleList;
import android.os.Looper;
import com.trebledot.stone.runtime.ArtApplication;
import java.lang.reflect.Method;

/** Exercises framework configuration services, resources and Activity state without desktop input. */
public final class ConfigurationProbe {
    private static final Object RETAINED = new Object();
    private static TestActivity current;
    private static int creates, callbacks;

    public static final class TestActivity extends Activity {
        int value;
        @Override protected void onCreate(Bundle saved) {
            super.onCreate(saved);
            current = this;
            creates++;
            value = saved == null ? 42 : saved.getInt("value");
            if (saved != null && getLastNonConfigurationInstance() != RETAINED)
                throw new AssertionError("configuration non-configuration state");
        }
        @Override protected void onSaveInstanceState(Bundle saved) {
            super.onSaveInstanceState(saved);
            saved.putInt("value", value);
        }
        @Override public Object onRetainNonConfigurationInstance() { return RETAINED; }
        @Override protected void onDestroy() { setIntent(null); super.onDestroy(); }
        @Override public void onConfigurationChanged(Configuration config) {
            super.onConfigurationChanged(config);
            callbacks++;
        }
    }

    private static final class Drained extends RuntimeException {}
    private static void drain() {
        Drained done = new Drained();
        new Handler(Looper.getMainLooper()).post(() -> { throw done; });
        try { Looper.loop(); }
        catch (Drained signal) { if (signal != done) throw signal; }
    }

    static void run(ArtApplication app) throws Exception {
        if (Boolean.getBoolean("stone.probe.locale-benchmark")) benchmarkLocales();
        LocaleManager locales = app.context.getSystemService(LocaleManager.class);
        UiModeManager mode = app.context.getSystemService(UiModeManager.class);
        LocaleList host = locales.getSystemLocales();
        Method appNight = Class.forName("com.trebledot.stone.runtime.ArtConfiguration").getDeclaredMethod("applicationNightMode");
        appNight.setAccessible(true);
        int originalMode = (Integer) appNight.invoke(null);
        LocaleList originalLocales = locales.getApplicationLocales();
        Class<?> configuration = Class.forName("com.trebledot.stone.runtime.ArtConfiguration");
        Method readNative = configuration.getDeclaredMethod("readHostState");
        Method read = configuration.getDeclaredMethod("read", String.class);
        Method apply = configuration.getDeclaredMethod("apply", boolean.class);
        readNative.setAccessible(true);
        read.setAccessible(true);
        apply.setAccessible(true);
        if (host.isEmpty() || (mode.getNightMode() != UiModeManager.MODE_NIGHT_YES && mode.getNightMode() != UiModeManager.MODE_NIGHT_NO))
            throw new AssertionError("Windows UI preferences are missing");
        locales.setApplicationLocales(LocaleList.getEmptyLocaleList());
        mode.setApplicationNightMode(-1);
        drain();
        Configuration initial = app.context.getResources().getConfiguration();
        // Resources may put a supported APK language first when a Windows language has no translation.
        if (initial.getLocales().indexOf(host.get(0)) < 0 || !host.equals(LocaleList.getDefault())
                || (initial.uiMode & Configuration.UI_MODE_NIGHT_MASK)
                != (mode.getNightMode() == UiModeManager.MODE_NIGHT_YES ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO))
            throw new AssertionError("follow Windows configuration: " + initial);
        int density = initial.densityDpi;
        ActivityInfo info = new ActivityInfo(app.apk.activityInfo);
        info.name = TestActivity.class.getName();
        info.targetActivity = null;
        info.configChanges = 0;
        Method launch = ArtApplication.class.getDeclaredMethod("launch", ActivityInfo.class, Intent.class, IBinder.class, String.class, int.class);
        launch.setAccessible(true);
        launch.invoke(app, info, new Intent().setClassName(app.context.getPackageName(), info.name), null, null, -1);
        try {
            int declaredOrientation = info.screenOrientation;
            if (current.getRequestedOrientation() != declaredOrientation)
                throw new AssertionError("initial manifest orientation request");
            int orientation = current.getResources().getConfiguration().orientation;
            java.util.concurrent.atomic.AtomicReference<Throwable> failure = new java.util.concurrent.atomic.AtomicReference<>();
            Thread renderer = new Thread(() -> {
                try {
                    for (int request : new int[]{ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, ActivityInfo.SCREEN_ORIENTATION_FULL_USER}) {
                        current.setRequestedOrientation(request);
                        if (current.getRequestedOrientation() != request) throw new AssertionError("render-thread orientation request");
                    }
                } catch (Throwable error) { failure.set(error); }
            }, "Orientation render thread");
            renderer.start(); renderer.join(3000);
            if (renderer.isAlive() || failure.get() != null) throw new AssertionError("native render-thread orientation API", failure.get());
            if (info.screenOrientation != declaredOrientation || current.getResources().getConfiguration().orientation != orientation)
                throw new AssertionError("orientation request changed manifest or desktop geometry");
            if (!(mode.getNightMode() == UiModeManager.MODE_NIGHT_YES ? "dark" : "light").equals(resource("configuration_probe_night")))
                throw new AssertionError("initial Windows night resource selection");
            current.value = 73;
            TestActivity previous = current;
            current.recreate();
            current.recreate();
            drain();
            if (current == previous || current.value != 73 || creates != 2)
                throw new AssertionError("explicit Activity.recreate and duplicate request coalescing");
            if (current.getRequestedOrientation() != ActivityInfo.SCREEN_ORIENTATION_FULL_USER)
                throw new AssertionError("orientation request lost during Activity recreation");
            previous = current;
            int beforeChanges = creates;
            locales.setApplicationLocales(LocaleList.forLanguageTags("ar,fr-FR"));
            drain();
            Configuration arabic = current.getResources().getConfiguration();
            if (current == previous || current.value != 73 || creates != beforeChanges + 1 || !"ar".equals(arabic.getLocales().get(0).getLanguage())
                    || arabic.getLayoutDirection() != android.view.View.LAYOUT_DIRECTION_RTL || arabic.densityDpi != density
                    || !"ar".equals(java.util.Locale.getDefault().getLanguage()) || !"arabic".equals(resource("configuration_probe_locale")))
                throw new AssertionError("locale Activity recreation and saved state: " + arabic);
            previous = current;
            int opposite = mode.getNightMode() == UiModeManager.MODE_NIGHT_YES ? UiModeManager.MODE_NIGHT_NO : UiModeManager.MODE_NIGHT_YES;
            mode.setApplicationNightMode(opposite);
            drain();
            if (current == previous || current.value != 73 || creates != beforeChanges + 2
                    || (current.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                        != (opposite == UiModeManager.MODE_NIGHT_YES ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO)
                    || !(opposite == UiModeManager.MODE_NIGHT_YES ? "dark" : "light").equals(resource("configuration_probe_night")))
                throw new AssertionError("night Activity recreation and saved state");
            info.configChanges = ActivityInfo.CONFIG_LOCALE | ActivityInfo.CONFIG_LAYOUT_DIRECTION | ActivityInfo.CONFIG_UI_MODE;
            previous = current;
            locales.setApplicationLocales(LocaleList.forLanguageTags("en-US"));
            drain();
            if (current != previous || callbacks != 1 || !"en-US".equals(current.getResources().getConfiguration().getLocales().get(0).toLanguageTag()))
                throw new AssertionError("handled Activity configuration callback");
            java.util.Properties saved = new java.util.Properties();
            try (java.io.FileInputStream input = new java.io.FileInputStream(new java.io.File(app.apk.applicationInfo.dataDir, ".stone-configuration.properties"))) {
                saved.load(input);
            }
            if (!"en-US".equals(saved.getProperty("locales")) || !Integer.toString(opposite).equals(saved.getProperty("night")))
                throw new AssertionError("persistent application UI overrides");
            java.lang.reflect.Field appLocales = configuration.getDeclaredField("appLocales");
            java.lang.reflect.Field nightMode = configuration.getDeclaredField("appNight");
            appLocales.setAccessible(true);
            nightMode.setAccessible(true);
            appLocales.set(null, LocaleList.getEmptyLocaleList());
            nightMode.setInt(null, -1);
            Method attach = configuration.getDeclaredMethod("attach", com.trebledot.stone.runtime.ArtPackage.class);
            attach.setAccessible(true);
            attach.invoke(null, app.apk);
            if (!"en-US".equals(locales.getApplicationLocales().toLanguageTags()) || (Integer) appNight.invoke(null) != opposite)
                throw new AssertionError("application UI override reload");
            locales.setApplicationLocales(LocaleList.getEmptyLocaleList());
            mode.setApplicationNightMode(-1);
            drain();
            read.invoke(null, "ja-JP|" + (opposite == UiModeManager.MODE_NIGHT_YES ? "dark" : "light"));
            apply.invoke(null, true);
            drain();
            if (!"ja-JP".equals(locales.getSystemLocales().toLanguageTags()) || mode.getNightMode() != opposite
                    || current.getResources().getConfiguration().getLocales().indexOf(java.util.Locale.JAPAN) < 0
                    || !"ja-JP".equals(LocaleList.getDefault().toLanguageTags()))
                throw new AssertionError("live Windows configuration update and framework cache invalidation");
        } finally {
            app.destroyActivity();
            read.invoke(null, readNative.invoke(null));
            locales.setApplicationLocales(originalLocales);
            mode.setApplicationNightMode(originalMode);
            apply.invoke(null, false);
            drain();
        }
        launch.invoke(app, info, new Intent().setClassName(app.context.getPackageName(), info.name), null, null, -1);
        try {
            if (current.getRequestedOrientation() != info.screenOrientation)
                throw new AssertionError("finished Activity leaked its orientation request to a new instance");
        } finally { app.destroyActivity(); }
        System.out.println("PASS Activity orientation: manifest default, render-thread set/get, unchanged desktop geometry, recreation retention and new-instance isolation");
        System.out.println("ART synchronized Windows locales/night, app overrides, RTL, configuration callbacks and saved Activity state.");
    }

    private static String resource(String name) {
        return current.getString(current.getResources().getIdentifier(name, "string", current.getPackageName()));
    }

    private static void benchmarkLocales() {
        String[] available = {"en-US", "fr-FR", "de-DE", "es-ES", "it-IT", "ru-RU", "ja-JP", "ko-KR", "pt-BR", "ar", "vi-VN", "zh-CN"};
        LocaleList preferred = LocaleList.forLanguageTags("zh-CN,en-US");
        long start = System.nanoTime();
        for (int index = 0; index < 5000; index++) {
            if (!java.util.Locale.SIMPLIFIED_CHINESE.equals(preferred.getFirstMatch(available)))
                throw new AssertionError("locale match benchmark result");
        }
        System.out.println("ART locale benchmark: 5000 lookups in " + ((System.nanoTime() - start) / 1000000) + " ms");
    }
}
