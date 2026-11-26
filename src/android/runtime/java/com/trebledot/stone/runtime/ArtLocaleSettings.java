package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.provider.Settings;

/** The application-owned locale settings entry; it never changes another package. */
final class ArtLocaleSettings {
    static ActivityInfo info(ArtPackage apk, ComponentName component) {
        if (component == null || !apk.applicationInfo.packageName.equals(component.getPackageName())
                || !ArtLocaleActivity.class.getName().equals(component.getClassName())) return null;
        ActivityInfo info = new ActivityInfo(apk.activityInfo);
        info.name = ArtLocaleActivity.class.getName(); info.targetActivity = null;
        info.exported = false; info.labelRes = 0; info.nonLocalizedLabel = "Stone";
        info.configChanges = ActivityInfo.CONFIG_LOCALE | ActivityInfo.CONFIG_LAYOUT_DIRECTION;
        info.theme = android.R.style.Theme_DeviceDefault_DayNight;
        return info;
    }
    static ActivityInfo resolve(ArtPackage apk, Intent intent) {
        if (!accepts(apk.applicationInfo.packageName,intent)) return null;
        ComponentName component = intent.getComponent();
        return info(apk,component == null ? new ComponentName(apk.applicationInfo.packageName,ArtLocaleActivity.class.getName()) : component);
    }
    static boolean accepts(String packageName, Intent intent) {
        if (intent == null || !Settings.ACTION_APP_LOCALE_SETTINGS.equals(intent.getAction()) || intent.getData() == null
                || !"package".equals(intent.getData().getScheme()) || intent.getData().getFragment() != null
                || !packageName.equals(intent.getData().getSchemeSpecificPart())
                || intent.getType() != null || intent.getSelector() != null || intent.getClipData() != null
                || intent.getExtras() != null && !intent.getExtras().isEmpty()
                || intent.getPackage() != null && !packageName.equals(intent.getPackage())) return false;
        if (intent.getCategories() != null) for (String category : intent.getCategories())
            if (!Intent.CATEGORY_DEFAULT.equals(category)) return false;
        ComponentName component = intent.getComponent();
        return component == null || packageName.equals(component.getPackageName()) && ArtLocaleActivity.class.getName().equals(component.getClassName());
    }
}
