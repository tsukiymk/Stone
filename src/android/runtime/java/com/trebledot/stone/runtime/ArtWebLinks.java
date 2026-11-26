package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.net.Uri;
import java.io.IOException;

/** HTTP/HTTPS VIEW intents use the Windows browser; no Android browser APK is loaded. */
final class ArtWebLinks {
    private static final String PACKAGE = "com.trebledot.stone.browser";
    private static final ComponentName COMPONENT = new ComponentName(PACKAGE,PACKAGE + ".BrowserActivity");
    private static native void openNative(String owner, String url) throws IOException;

    static ActivityInfo activityInfo(ComponentName component) {
        if (!COMPONENT.equals(component)) return null;
        ActivityInfo info = new ActivityInfo(); info.packageName = PACKAGE; info.name = COMPONENT.getClassName();
        info.enabled = true; info.exported = true; info.nonLocalizedLabel = ArtText.get(ArtStrings.windows_browser);
        info.applicationInfo = new ApplicationInfo(); info.applicationInfo.packageName = PACKAGE;
        info.applicationInfo.enabled = true; info.applicationInfo.nonLocalizedLabel = info.nonLocalizedLabel;
        return info;
    }
    static ActivityInfo resolve(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction()) || intent.getSelector() != null
                || intent.getType() != null || intent.getClipData() != null || intent.getExtras() != null
                || intent.getComponent() != null && !COMPONENT.equals(intent.getComponent())
                || intent.getPackage() != null && !PACKAGE.equals(intent.getPackage())) return null;
        if (intent.getCategories() != null) for (String category : intent.getCategories())
            if (!Intent.CATEGORY_DEFAULT.equals(category) && !Intent.CATEGORY_BROWSABLE.equals(category)) return null;
        Uri uri = intent.getData();
        if (uri == null || !("http".equalsIgnoreCase(uri.getScheme()) || "https".equalsIgnoreCase(uri.getScheme()))
                || !uri.isHierarchical() || uri.getHost() == null || uri.getHost().isEmpty() || uri.getUserInfo() != null) return null;
        String text = uri.toString();
        if (text.length() > 8192 || !text.regionMatches(true,0,uri.getScheme() + "://",0,uri.getScheme().length() + 3)) return null;
        for (int index = 0; index < text.length(); ++index) {
            char ch = text.charAt(index);
            if (ch <= ' ' || ch == 0x7f || ch == '\\' || ch == '"' || ch == '<' || ch == '>') return null;
        }
        return activityInfo(COMPONENT);
    }
    static int start(ArtApplication application, Intent intent, int request) {
        if (request >= 0 || resolve(intent) == null) return -91;
        if (ArtServices.activities.importance() > android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND) return -94;
        try { openNative(application.apk.applicationInfo.packageName,intent.getDataString()); return 0; }
        catch (IOException error) { throw new IllegalStateException("Could not open the Windows web browser",error); }
    }
}
