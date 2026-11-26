package com.trebledot.stone.runtime;

import android.content.ClipData;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import java.io.IOException;

/** Plain-text SEND/CHOOSER intents use the Windows share UI for the live Client window. */
final class ArtShares {
    private static final String PACKAGE = "com.trebledot.stone.share";
    private static final ComponentName COMPONENT = new ComponentName(PACKAGE,PACKAGE + ".ShareActivity");
    private static native void showNative(String title, String text) throws IOException;
    static ActivityInfo activityInfo(ComponentName component) {
        if (!COMPONENT.equals(component)) return null;
        ActivityInfo info = new ActivityInfo(); info.packageName = PACKAGE; info.name = COMPONENT.getClassName();
        info.enabled = true; info.exported = true; info.nonLocalizedLabel = ArtText.get(ArtStrings.windows_share);
        info.applicationInfo = new ApplicationInfo(); info.applicationInfo.packageName = PACKAGE;
        info.applicationInfo.enabled = true; info.applicationInfo.nonLocalizedLabel = info.nonLocalizedLabel; return info;
    }
    private static boolean target(Intent intent) {
        if (intent.getCategories() != null) for (String category : intent.getCategories()) if (!Intent.CATEGORY_DEFAULT.equals(category)) return false;
        return intent.getSelector() == null && intent.getData() == null
            && (intent.getComponent() == null || COMPONENT.equals(intent.getComponent()))
            && (intent.getPackage() == null || PACKAGE.equals(intent.getPackage()));
    }
    static String[] content(Intent original) {
        if (original == null || !target(original)) return null;
        Intent send = original;
        if (Intent.ACTION_CHOOSER.equals(original.getAction())) {
            if (original.getType() != null) return null;
            Bundle extras = original.getExtras();
            if (extras == null) return null;
            for (String key : extras.keySet()) if (!Intent.EXTRA_INTENT.equals(key) && !Intent.EXTRA_TITLE.equals(key)) return null;
            Object caption = extras.get(Intent.EXTRA_TITLE);
            if (caption != null && !(caption instanceof CharSequence)) return null;
            Object inner = extras.get(Intent.EXTRA_INTENT);
            if (!(inner instanceof Intent)) return null; send = (Intent) inner;
            if (!target(send)) return null;
        }
        if (!Intent.ACTION_SEND.equals(send.getAction()) || !"text/plain".equals(send.getType())) return null;
        Bundle extras = send.getExtras();
        if (extras == null) return null;
        for (String key : extras.keySet()) if (!Intent.EXTRA_TEXT.equals(key) && !Intent.EXTRA_SUBJECT.equals(key) && !Intent.EXTRA_TITLE.equals(key)) return null;
        for (String key : new String[]{Intent.EXTRA_TEXT,Intent.EXTRA_SUBJECT,Intent.EXTRA_TITLE}) {
            Object value = extras.get(key); if (value != null && !(value instanceof CharSequence)) return null;
        }
        CharSequence text = send.getCharSequenceExtra(Intent.EXTRA_TEXT);
        CharSequence title = send.getCharSequenceExtra(Intent.EXTRA_SUBJECT);
        if (title == null || title.length() == 0) title = send.getCharSequenceExtra(Intent.EXTRA_TITLE);
        if ((title == null || title.length() == 0) && original != send) title = original.getCharSequenceExtra(Intent.EXTRA_TITLE);
        if (text == null || text.length() == 0 || text.length() > 8192 || text.toString().indexOf('\0') >= 0
                || title != null && (title.length() > 1024 || title.toString().indexOf('\0') >= 0)) return null;
        // Framework migration may duplicate EXTRA_TEXT in ClipData. Accept only
        // that exact representation, never discard a URI, HTML or Intent grant.
        for (ClipData clip : new ClipData[]{original.getClipData(),send.getClipData()}) if (clip != null) {
            if (clip.getItemCount() != 1) return null; ClipData.Item item = clip.getItemAt(0);
            if (item.getUri() != null || item.getIntent() != null || item.getHtmlText() != null
                    || item.getText() == null || !text.toString().contentEquals(item.getText())) return null;
        }
        return new String[]{title == null ? "" : title.toString(),text.toString()};
    }
    static ActivityInfo resolve(Intent intent) { return content(intent) == null && ArtShareFiles.parse(intent) == null ? null : activityInfo(COMPONENT); }
    static int start(ArtApplication application, Intent intent, int request) {
        String[] content = content(intent);
        if (content == null) return ArtShareFiles.start(application,intent,request);
        if (request >= 0) return -91;
        if (ArtServices.activities.importance() > android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND) return -94;
        if (content[0].isEmpty()) content[0] = application.apk.applicationInfo.loadLabel(application.context.getPackageManager()).toString();
        if (content[0].isEmpty() || content[0].length() > 1024) content[0] = "Stone";
        try { showNative(content[0],content[1]); return 0; }
        catch (IOException error) { throw new IllegalStateException("Could not show Windows share UI",error); }
    }
}
