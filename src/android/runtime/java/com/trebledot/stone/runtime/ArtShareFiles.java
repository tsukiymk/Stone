package com.trebledot.stone.runtime;

import android.content.ClipData;
import android.content.Intent;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Read-granted, app-owned APK content URIs become immutable Windows snapshots. */
final class ArtShareFiles {
    private static final String MIME = "application/vnd.android.package-archive";
    private static final AtomicBoolean busy = new AtomicBoolean();
    private static native void showNative(String title, String name, int descriptor) throws IOException;
    static final class Request {
        final Uri uri;
        final String title;
        Request(Uri uri, String title) { this.uri = uri; this.title = title; }
    }
    private static boolean target(Intent intent) {
        if (intent.getSelector() != null || intent.getPackage() != null && !"com.trebledot.stone.share".equals(intent.getPackage())
                || intent.getComponent() != null && ArtShares.activityInfo(intent.getComponent()) == null) return false;
        if (intent.getCategories() != null) for (String category : intent.getCategories()) if (!Intent.CATEGORY_DEFAULT.equals(category)) return false;
        return (intent.getFlags() & (Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)) == 0;
    }
    static Request parse(Intent original) {
        if (original == null || !target(original)) return null;
        Intent send = original;
        CharSequence chooserTitle = null;
        if (Intent.ACTION_CHOOSER.equals(original.getAction())) {
            if (original.getType() != null || original.getData() != null) return null;
            Bundle extras = original.getExtras(); if (extras == null) return null;
            for (String key : extras.keySet()) if (!Intent.EXTRA_INTENT.equals(key) && !Intent.EXTRA_TITLE.equals(key)) return null;
            Object inner = extras.get(Intent.EXTRA_INTENT), title = extras.get(Intent.EXTRA_TITLE);
            if (!(inner instanceof Intent) || title != null && !(title instanceof CharSequence)) return null;
            send = (Intent) inner; chooserTitle = (CharSequence) title;
            if (!target(send)) return null;
        }
        if (!Intent.ACTION_SEND.equals(send.getAction()) || !MIME.equals(send.getType())
                || (send.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0) return null;
        Bundle extras = send.getExtras(); if (extras == null) return null;
        for (String key : extras.keySet()) if (!Intent.EXTRA_STREAM.equals(key) && !Intent.EXTRA_TITLE.equals(key) && !Intent.EXTRA_SUBJECT.equals(key)) return null;
        Object stream = extras.get(Intent.EXTRA_STREAM); if (!(stream instanceof Uri)) return null;
        Uri uri = (Uri) stream;
        if (!"content".equals(uri.getScheme()) || uri.getAuthority() == null || uri.getAuthority().isEmpty()
                || uri.getAuthority().contains("@") || uri.getFragment() != null || uri.toString().length() > 8192
                || uri.toString().indexOf('\0') >= 0 || send.getData() != null && !uri.equals(send.getData())) return null;
        CharSequence title = chooserTitle;
        for (String key : new String[]{Intent.EXTRA_TITLE,Intent.EXTRA_SUBJECT}) {
            Object value = extras.get(key);
            if (value != null && !(value instanceof CharSequence)) return null;
            if (value != null && ((CharSequence) value).length() > 0) title = (CharSequence) value;
        }
        if (title != null && (title.length() > 1024 || title.toString().indexOf('\0') >= 0)) return null;
        for (ClipData clip : new ClipData[]{original.getClipData(),send.getClipData()}) if (clip != null) {
            if (clip.getItemCount() != 1) return null;
            ClipData.Item item = clip.getItemAt(0);
            if (!uri.equals(item.getUri()) || item.getText() != null || item.getHtmlText() != null || item.getIntent() != null) return null;
        }
        return new Request(uri,title == null ? "" : title.toString());
    }
    static String filename(String displayName) {
        if (displayName == null || displayName.length() <= 4 || displayName.length() > 128 || displayName.startsWith(".")
                || !displayName.endsWith(".apk") || !displayName.matches("[A-Za-z0-9._-]+")
                || displayName.matches("(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\\..*")) return "shared.apk";
        return displayName;
    }
    static int start(ArtApplication application, Intent intent, int requestCode) {
        Request request = parse(intent);
        if (request == null || requestCode >= 0) return -91;
        if (ArtServices.activities.importance() > android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND) return -94;
        ProviderInfo provider = application.context.getPackageManager().resolveContentProvider(request.uri.getAuthority(),0);
        if (provider == null || !provider.enabled || !application.apk.applicationInfo.packageName.equals(provider.packageName)) return -94;
        if (!busy.compareAndSet(false,true)) throw new IllegalStateException("APK share preparation is already running");
        Thread worker = new Thread(() -> {
            CancellationSignal cancellation = new CancellationSignal();
            Handler main = new Handler(Looper.getMainLooper());
            Runnable timeout = cancellation::cancel; main.postDelayed(timeout,30000);
            try {
                String name = request.uri.getLastPathSegment();
                try (Cursor cursor = application.context.getContentResolver().query(request.uri,new String[]{OpenableColumns.DISPLAY_NAME},null,null,null,cancellation)) {
                    if (cursor != null && cursor.moveToFirst()) {
                        int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME); if (column >= 0) name = cursor.getString(column);
                    }
                }
                String title = request.title;
                if (title.isEmpty()) title = application.apk.applicationInfo.loadLabel(application.context.getPackageManager()).toString();
                if (title.isEmpty() || title.length() > 1024 || title.indexOf('\0') >= 0) title = "Stone";
                try (android.content.res.AssetFileDescriptor file = application.context.getContentResolver().openAssetFileDescriptor(request.uri,"r",cancellation)) {
                    if (file == null || file.getStartOffset() != 0) throw new IOException("APK provider did not return a complete file");
                    long size = file.getParcelFileDescriptor().getStatSize();
                    if (size <= 0 || size > 1024L*1024*1024 || file.getDeclaredLength() >= 0 && file.getDeclaredLength() != size)
                        throw new IOException("APK sharing requires a regular complete file of at most 1 GiB");
                    cancellation.throwIfCanceled();
                    showNative(title,filename(name),file.getParcelFileDescriptor().getFd());
                }
            } catch (Exception error) {
                android.util.Log.e("StoneShare","Could not prepare APK share",error);
                main.post(() -> {
                    android.app.Activity activity = application.currentActivity(null);
                    if (activity != null && !activity.isFinishing()) new android.app.AlertDialog.Builder(activity)
                        .setTitle("Stone").setMessage(ArtText.get(activity,ArtStrings.share_prepare_error)).setPositiveButton(android.R.string.ok,null).show();
                });
            } finally { main.removeCallbacks(timeout); busy.set(false); }
        },"Stone-APK-share");
        worker.setDaemon(true);
        try { worker.start(); } catch (RuntimeException | Error error) { busy.set(false); throw error; }
        return 0;
    }
}
