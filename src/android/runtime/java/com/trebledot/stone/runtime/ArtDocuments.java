package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.UUID;

/** Current-session grants to files explicitly chosen in the Windows save dialog. */
final class ArtDocuments {
    static final String AUTHORITY = "com.trebledot.stone.documents";
    static native long createNative(String name, String mime) throws IOException;
    static native String[] pickNative(long request) throws IOException;
    static native void cancelNative(long request) throws IOException;
    static native void releaseNative(long request);
    static native void releaseGrantNative(long grant);
    private static native int openNative(long grant, int mode) throws IOException;
    private static native long sizeNative(long grant) throws IOException;
    private static final LinkedHashMap<Uri, Grant> grants = new LinkedHashMap<>();
    static final class Grant {
        final long id;
        final String name, mime;
        Grant(long id, String name, String mime) { this.id = id; this.name = name; this.mime = mime; }
        long size() throws IOException { return sizeNative(id); }
        ParcelFileDescriptor open(String mode) throws IOException { return ParcelFileDescriptor.adoptFd(openNative(id,ParcelFileDescriptor.parseMode(mode))); }
    }
    static synchronized Uri publish(long id, String name, String mime) {
        if (id <= 0 || name == null || mime == null) throw new IllegalArgumentException("Invalid document grant");
        while (grants.size() >= 32) {
            Uri oldest = grants.keySet().iterator().next(); releaseGrantNative(grants.remove(oldest).id);
        }
        Uri uri = new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath("document").appendPath(UUID.randomUUID().toString()).build();
        grants.put(uri,new Grant(id,name,mime)); return uri;
    }
    static synchronized Grant require(Uri uri) {
        Grant grant = grants.get(uri);
        if (grant == null || !"content".equals(uri.getScheme()) || !AUTHORITY.equals(uri.getAuthority())
                || uri.getQuery() != null || uri.getFragment() != null) throw new IllegalArgumentException("Unknown document URI");
        return grant;
    }
    static ActivityInfo info(ArtPackage apk, ComponentName component) {
        if (component == null || !apk.applicationInfo.packageName.equals(component.getPackageName())
                || !ArtDocumentActivity.class.getName().equals(component.getClassName())) return null;
        ActivityInfo info = new ActivityInfo(apk.activityInfo); info.name = ArtDocumentActivity.class.getName(); info.targetActivity = null;
        info.exported = false; info.labelRes = 0; info.nonLocalizedLabel = "Stone";
        info.configChanges = ActivityInfo.CONFIG_LOCALE | ActivityInfo.CONFIG_LAYOUT_DIRECTION | ActivityInfo.CONFIG_UI_MODE;
        info.theme = android.R.style.Theme_DeviceDefault_DayNight; return info;
    }
    static ActivityInfo resolve(ArtPackage apk, Intent intent) {
        if (!accepts(apk.applicationInfo.packageName,intent)) return null;
        return info(apk,new ComponentName(apk.applicationInfo.packageName,ArtDocumentActivity.class.getName()));
    }
    static boolean accepts(String packageName, Intent intent) {
        if (ArtDocumentTrees.accepts(packageName,intent)) return true;
        if (intent == null || !Intent.ACTION_CREATE_DOCUMENT.equals(intent.getAction()) || intent.getData() != null
                || intent.getSelector() != null || intent.getClipData() != null || intent.getType() == null
                || intent.getType().length() > 255 || !intent.getType().matches("[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+")
                || intent.getPackage() != null && !packageName.equals(intent.getPackage())) return false;
        if (intent.getCategories() != null) for (String category : intent.getCategories())
            if (!Intent.CATEGORY_OPENABLE.equals(category) && !Intent.CATEGORY_DEFAULT.equals(category)) return false;
        ComponentName component = intent.getComponent();
        if (component != null && (!packageName.equals(component.getPackageName()) || !ArtDocumentActivity.class.getName().equals(component.getClassName()))) return false;
        Bundle extras = intent.getExtras();
        if (extras != null) for (String key : extras.keySet()) {
            if (!Intent.EXTRA_TITLE.equals(key) || !(extras.get(key) instanceof String) || ((String) extras.get(key)).length() > 1024) return false;
        }
        return true;
    }
    static String filename(Intent intent) {
        String value = intent.getStringExtra(Intent.EXTRA_TITLE);
        if (value == null) value = "document";
        value = value.replaceAll("[\\x00-\\x1f<>:\"/\\\\|?*]","_").replaceAll("[ .]+$","");
        if (value.isEmpty() || value.matches("(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?")) value = "document";
        if (value.length() > 255) value = value.substring(0,255);
        return value;
    }
}
