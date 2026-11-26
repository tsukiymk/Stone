package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.provider.Settings;

/** The current application's special shared-storage access settings. */
final class ArtStorageAccess {
    static boolean accepts(String packageName,Intent intent) {
        if (intent == null || (!Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION.equals(intent.getAction())
                && !Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION.equals(intent.getAction()))
                || intent.getType()!=null || intent.getSelector()!=null || intent.getClipData()!=null
                || intent.getExtras()!=null && !intent.getExtras().isEmpty()
                || intent.getPackage()!=null && !packageName.equals(intent.getPackage())) return false;
        if (Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION.equals(intent.getAction())) {
            android.net.Uri data=intent.getData();
            if (data==null || !"package".equals(data.getScheme()) || data.getFragment()!=null
                    || !packageName.equals(data.getSchemeSpecificPart())) return false;
        } else if (intent.getData()!=null) return false;
        if (intent.getCategories()!=null) for (String category:intent.getCategories())
            if (!Intent.CATEGORY_DEFAULT.equals(category)) return false;
        ComponentName component=intent.getComponent();
        return component==null || packageName.equals(component.getPackageName()) && ArtStorageAccessActivity.class.getName().equals(component.getClassName());
    }
    static ActivityInfo info(ArtPackage apk,ComponentName component) {
        if (component==null || !apk.applicationInfo.packageName.equals(component.getPackageName())
                || !ArtStorageAccessActivity.class.getName().equals(component.getClassName())) return null;
        ActivityInfo info=new ActivityInfo(apk.activityInfo);
        info.name=ArtStorageAccessActivity.class.getName(); info.targetActivity=null; info.exported=false;
        info.configChanges=0; info.labelRes=0; info.nonLocalizedLabel="Stone";
        info.theme=android.R.style.Theme_DeviceDefault_DayNight;
        return info;
    }
    static ActivityInfo resolve(ArtPackage apk,Intent intent) {
        return accepts(apk.applicationInfo.packageName,intent)
            ? info(apk,new ComponentName(apk.applicationInfo.packageName,ArtStorageAccessActivity.class.getName())) : null;
    }
}
