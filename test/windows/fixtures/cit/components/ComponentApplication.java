package com.trebledot.stone.cit.components;

import android.app.Application;

public final class ComponentApplication extends Application {
    static boolean providerReady;
    private Thread.UncaughtExceptionHandler applicationHandler;

    @Override protected void attachBaseContext(android.content.Context context) {
        super.attachBaseContext(context);
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        if (previous == null) throw new AssertionError("Android crash handler missing before Application.attachBaseContext");
        applicationHandler = (thread, error) -> previous.uncaughtException(thread, error);
        Thread.setDefaultUncaughtExceptionHandler(applicationHandler);
    }

    @Override public void onCreate() {
        super.onCreate();
        if (Thread.getDefaultUncaughtExceptionHandler() != applicationHandler)
            throw new AssertionError("Application crash handler was replaced during startup");
        android.location.LocationManager location = getSystemService(android.location.LocationManager.class);
        if (location == null || !location.getAllProviders().isEmpty()
                || !location.getProviders(true).isEmpty() || location.isLocationEnabled()
                || location.isProviderEnabled(android.location.LocationManager.GPS_PROVIDER))
            throw new AssertionError("Host without location providers must expose a disabled LocationManager");
        android.appwidget.AppWidgetManager widgets = android.appwidget.AppWidgetManager.getInstance(this);
        if (widgets == null || widgets.getAppWidgetIds(new android.content.ComponentName(this, ComponentProvider.class)).length != 0
                || widgets.isRequestPinAppWidgetSupported())
            throw new AssertionError("Host without widgets must expose no bound IDs or pin support");
        providerReady = ComponentProvider.creates == 1 && getContentResolver().call(
                ComponentProvider.ALIAS, "authority", null, null) != null;
        android.util.Log.i("StoneCIT", "application crash-handler PASS");
        android.util.Log.i("StoneCIT", "location unavailable PASS");
        android.util.Log.i("StoneCIT", "widgets unavailable PASS");
    }
}
