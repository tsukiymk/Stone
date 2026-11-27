package com.trebledot.stone.runtime;

import android.app.UiModeManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.LocaleList;
import android.os.SystemClock;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.util.Properties;

/** Windows UI preferences and this application's explicit Android overrides. */
public final class ArtConfiguration {
    private static native String readHostState();
    private static native int readDisplayDensity();
    private static String hostState;
    private static LocaleList hostLocales = LocaleList.getEmptyLocaleList();
    private static boolean hostDark;
    private static LocaleList appLocales = LocaleList.getEmptyLocaleList();
    private static int appNight = -1;
    private static ArtPackage apk;
    private static int sequence = 1;
    private static int windowWidth, windowHeight;
    private static int displayDensity;
    private static long nextPoll;
    private static final java.util.concurrent.atomic.AtomicInteger backgroundSaves = new java.util.concurrent.atomic.AtomicInteger();
    private ArtConfiguration() {}
    static void beginSave() { backgroundSaves.incrementAndGet(); }
    static void endSave() { backgroundSaves.decrementAndGet(); }
    static boolean hasPendingSave() { return backgroundSaves.get() != 0; }

    public static synchronized void initialize() {
        if (hostState == null) {
            read(readHostState());
            displayDensity = readDisplayDensity();
            apply(false);
        }
    }

    static synchronized void attach(ArtPackage value) {
        initialize();
        apk = value;
        float density = Resources.getSystem().getDisplayMetrics().density;
        windowWidth = Math.max(1, Math.round(360 * density));
        windowHeight = Math.max(1, Math.round(640 * density));
        Properties preferences = new Properties();
        File file = preferences();
        if (file.isFile()) try (FileInputStream input = new FileInputStream(file)) {
            preferences.load(input);
            appLocales = LocaleList.forLanguageTags(preferences.getProperty("locales", ""));
            int night = Integer.parseInt(preferences.getProperty("night", "-1"));
            if (night == -1 || night == UiModeManager.MODE_NIGHT_AUTO || night == UiModeManager.MODE_NIGHT_NO || night == UiModeManager.MODE_NIGHT_YES)
                appNight = night;
        } catch (java.io.IOException | IllegalArgumentException error) {
            Log.w("StoneConfiguration", "Could not read application configuration", error);
        }
        apply(false);
    }

    private static void read(String state) {
        if (state == null) throw new IllegalStateException("Windows UI configuration is missing");
        String[] fields = state.split("\\|", -1);
        if (fields.length != 2 || (!fields[1].equals("dark") && !fields[1].equals("light")))
            throw new IllegalStateException("Invalid Windows UI configuration");
        LocaleList locales = LocaleList.forLanguageTags(fields[0]);
        if (locales.isEmpty()) throw new IllegalStateException("Windows UI language list is empty");
        hostLocales = locales;
        hostDark = fields[1].equals("dark");
        hostState = state;
    }

    static synchronized void poll() {
        long now = SystemClock.uptimeMillis();
        if (now < nextPoll) return;
        nextPoll = now + 1000;
        String state = readHostState();
        if (!state.equals(hostState)) { read(state); apply(true); }
    }

    static synchronized LocaleList systemLocales() { initialize(); return hostLocales; }
    static synchronized LocaleList applicationLocales() { return appLocales; }
    static synchronized int systemNightMode() { initialize(); return hostDark ? UiModeManager.MODE_NIGHT_YES : UiModeManager.MODE_NIGHT_NO; }
    static synchronized int applicationNightMode() { return appNight; }
    static Configuration current() { return new Configuration(Resources.getSystem().getConfiguration()); }
    static boolean resizableWindow() { return apk != null && apk.resizableWindow(); }

    static synchronized void resize(int width, int height) {
        int density = readDisplayDensity();
        if (windowWidth == width && windowHeight == height && displayDensity == density) return;
        windowWidth = width; windowHeight = height;
        displayDensity = density;
        apply(true);
    }

    private static void geometry(Configuration config, android.util.DisplayMetrics metrics) {
        if (windowWidth <= 0 || windowHeight <= 0) return;
        metrics.widthPixels = windowWidth; metrics.heightPixels = windowHeight;
        config.screenWidthDp = Math.max(1, (int) (windowWidth / metrics.density));
        config.screenHeightDp = Math.max(1, (int) (windowHeight / metrics.density));
        config.smallestScreenWidthDp = Math.min(config.screenWidthDp, config.screenHeightDp);
        config.orientation = windowWidth > windowHeight ? Configuration.ORIENTATION_LANDSCAPE : Configuration.ORIENTATION_PORTRAIT;
        try {
            int layout = (Integer) Configuration.class.getMethod("resetScreenLayout", int.class).invoke(null, config.screenLayout);
            config.screenLayout = (Integer) Configuration.class.getMethod("reduceScreenLayout", int.class, int.class, int.class)
                .invoke(null, layout, Math.max(config.screenWidthDp, config.screenHeightDp), config.smallestScreenWidthDp);
            Object window = Configuration.class.getField("windowConfiguration").get(config);
            android.graphics.Rect bounds = new android.graphics.Rect(0, 0, windowWidth, windowHeight);
            for (String method : new String[]{"setBounds", "setAppBounds", "setMaxBounds"})
                window.getClass().getMethod(method, android.graphics.Rect.class).invoke(window, bounds);
            window.getClass().getMethod("setWindowingMode", int.class).invoke(window, resizableWindow() ? 5 : 1);
            if (ArtServices.display != null) ArtServices.display.resize(windowWidth, windowHeight, displayDensity);
        } catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not update ART window geometry", error); }
    }

    static synchronized void setApplicationLocales(LocaleList locales) {
        if (locales == null) throw new NullPointerException("locales");
        if (appLocales.equals(locales)) return;
        save(locales, appNight);
        appLocales = locales;
        apply(true);
    }

    static synchronized void setApplicationNightMode(int mode) {
        if (mode != -1 && mode != UiModeManager.MODE_NIGHT_AUTO && mode != UiModeManager.MODE_NIGHT_NO && mode != UiModeManager.MODE_NIGHT_YES)
            throw new IllegalArgumentException("Unsupported application night mode: " + mode);
        if (appNight == mode) return;
        save(appLocales, mode);
        appNight = mode;
        apply(true);
    }

    private static File preferences() { return new File(apk.applicationInfo.dataDir, ".stone-configuration.properties"); }
    private static void save(LocaleList locales, int night) {
        if (apk == null) return;
        Properties preferences = new Properties();
        preferences.setProperty("locales", locales.toLanguageTags());
        preferences.setProperty("night", Integer.toString(night));
        File file = preferences();
        File pending = new File(file.getPath() + ".tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(pending)) {
                preferences.store(output, "Stone application UI configuration");
                output.getFD().sync();
            }
            android.system.Os.rename(pending.getPath(), file.getPath());
        } catch (java.io.IOException | android.system.ErrnoException error) {
            throw new IllegalStateException("Could not save application configuration", error);
        } finally { pending.delete(); }
    }

    private static void apply(boolean dispatch) {
        java.util.ArrayList<java.util.Locale> ordered = new java.util.ArrayList<>();
        for (LocaleList source : new LocaleList[]{appLocales, hostLocales}) {
            for (int index = 0; index < source.size(); index++) {
                java.util.Locale locale = source.get(index);
                if (!ordered.contains(locale)) ordered.add(locale);
            }
        }
        LocaleList locales = new LocaleList(ordered.toArray(new java.util.Locale[0]));
        LocaleList.setDefault(locales);
        boolean dark = appNight == UiModeManager.MODE_NIGHT_YES
            || ((appNight == -1 || appNight == UiModeManager.MODE_NIGHT_AUTO) && hostDark);
        Configuration previous = current();
        Configuration config = new Configuration(previous);
        android.util.DisplayMetrics metrics = new android.util.DisplayMetrics();
        metrics.setTo(Resources.getSystem().getDisplayMetrics());
        config.densityDpi = metrics.densityDpi = displayDensity;
        metrics.density = displayDensity / 160.0f;
        metrics.scaledDensity = metrics.density * config.fontScale;
        metrics.xdpi = metrics.ydpi = displayDensity * (96.0f / 160.0f);
        geometry(config, metrics);
        config.setLocales(locales);
        config.uiMode = (config.uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
            | (dark ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO);
        try { Configuration.class.getField("seq").setInt(config, ++sequence); }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not sequence ART configuration", error); }
        if (apk != null) try {
            Class<?> type = Class.forName("android.app.ResourcesManager");
            Object manager = type.getMethod("getInstance").invoke(null);
            Method update = type.getMethod("applyConfigurationToResources", Configuration.class,
                Class.forName("android.content.res.CompatibilityInfo"));
            update.invoke(manager, config, null);
            apk.resources.updateConfiguration(config, metrics);
        } catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not apply ART configuration", error); }
        Resources.getSystem().updateConfiguration(config, metrics);
        if(ArtServices.display!=null) try { ArtServices.display.configurationChanged(config); }
        catch(ReflectiveOperationException error) { throw new IllegalStateException("Could not update window context configuration",error); }
        if(apk!=null) try {
            // Compose's cached text images already contain density-scaled
            // pixels. New bitmap defaults must match the drawing Canvas so
            // drawBitmap does not apply that density a second time.
            android.graphics.Bitmap.class.getMethod("setDefaultDensity",int.class).invoke(null,displayDensity);
        } catch(ReflectiveOperationException error) { throw new IllegalStateException("Could not update bitmap display density",error); }
        if (apk != null) try { UiModeManager.class.getMethod("invalidateNightModeCache").invoke(null); }
        catch (NoSuchMethodException olderImage) { /* Earlier Android images have no night-mode Binder cache. */ }
        catch (ReflectiveOperationException error) { throw new IllegalStateException("Could not invalidate the ART night-mode cache", error); }
        if (dispatch && apk != null) ArtServices.activities.configurationChanged(config, previous.diff(config));
    }
}
