package com.trebledot.stone.cit.components;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;

public class ComponentProvider extends ContentProvider {
    static final String PRIMARY = "com.trebledot.stone.cit.components.primary";
    static final String ALIAS = "com.trebledot.stone.cit.components.alias";
    static final Uri ROWS = Uri.parse("content://" + PRIMARY + "/rows");
    static final Uri ALIAS_ROWS = Uri.parse("content://" + ALIAS + "/rows");
    static int creates;
    private String value;

    @Override public boolean onCreate() { creates++; return true; }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        MatrixCursor cursor = new MatrixCursor(new String[]{"value"});
        if (value != null) cursor.addRow(new Object[]{value});
        return cursor;
    }
    @Override public String getType(Uri uri) { return "vnd.android.cursor.dir/vnd.stone.component"; }
    @Override public Uri insert(Uri uri, ContentValues values) {
        update(uri, values, null, null);
        return Uri.withAppendedPath(uri, "1");
    }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) {
        value = values.getAsString("value");
        getContext().getContentResolver().notifyChange(Uri.withAppendedPath(ROWS, "1"), null);
        return 1;
    }
    @Override public int delete(Uri uri, String selection, String[] args) {
        int count = value == null ? 0 : 1;
        value = null;
        return count;
    }
    @Override public Bundle call(String authority, String method, String arg, Bundle extras) {
        Bundle result = new Bundle();
        result.putString("authority", authority);
        return result;
    }
    @Override public boolean refresh(Uri uri, Bundle extras, CancellationSignal signal) {
        return extras != null && extras.getBoolean("refresh");
    }

    public static final class Disabled extends ComponentProvider {
        @Override public boolean onCreate() {
            creates += 100;
            return true;
        }
    }

    public static final class Remote extends ComponentProvider {
        @Override public boolean onCreate() {
            throw new AssertionError("The remote provider must not be loaded in the main process");
        }
    }
}
