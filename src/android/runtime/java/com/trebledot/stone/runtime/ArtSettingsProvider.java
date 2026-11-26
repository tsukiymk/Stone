package com.trebledot.stone.runtime;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;

/** Read-only settings for the Windows host. Unconfigured settings use framework defaults. */
public final class ArtSettingsProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }

    @Override public Bundle call(String method, String name, Bundle extras) {
        if ("LIST_config".equals(method)) {
            Bundle result = new Bundle();
            result.putSerializable("value", new java.util.HashMap<String, String>());
            return result;
        }
        if (!"GET_system".equals(method) && !"GET_secure".equals(method) && !"GET_global".equals(method) && !"GET_config".equals(method))
            throw new UnsupportedOperationException("ART settings call: " + method);
        Bundle result = new Bundle();
        result.putString("value", null);
        return result;
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] arguments, String order) {
        checkUri(uri);
        return new MatrixCursor(projection == null ? new String[]{"name", "value"} : projection);
    }

    @Override public String getType(Uri uri) {
        checkUri(uri);
        return uri.getPathSegments().size() > 1 ? "vnd.android.cursor.item/setting" : "vnd.android.cursor.dir/setting";
    }

    private static void checkUri(Uri uri) {
        if (!"settings".equals(uri.getAuthority()) || uri.getPathSegments().isEmpty())
            throw new IllegalArgumentException("Invalid settings URI: " + uri);
        String table = uri.getPathSegments().get(0);
        if (!"system".equals(table) && !"secure".equals(table) && !"global".equals(table))
            throw new UnsupportedOperationException("ART settings table: " + table);
    }

    @Override public Uri insert(Uri uri, ContentValues values) { throw readOnly(); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] arguments) { throw readOnly(); }
    @Override public int delete(Uri uri, String selection, String[] arguments) { throw readOnly(); }
    private static SecurityException readOnly() { return new SecurityException("Windows host settings are read-only"); }
}
