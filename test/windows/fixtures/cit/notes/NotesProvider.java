package com.trebledot.stone.cit.notes;

import android.content.ContentProvider;
import android.content.ContentUris;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.net.Uri;
import android.os.Bundle;
import java.io.File;

public final class NotesProvider extends ContentProvider {
    static final Uri NOTES = Uri.parse("content://com.trebledot.stone.cit.notes.provider/notes");
    static final String BODY = "body";
    private static final String TABLE = "notes";
    private SQLiteOpenHelper helper;

    @Override public boolean onCreate() {
        helper = new SQLiteOpenHelper(getContext(), "笔记-📝.db", null, 1) {
            @Override public void onCreate(SQLiteDatabase database) {
                database.execSQL("CREATE TABLE notes (_id INTEGER PRIMARY KEY, body TEXT COLLATE LOCALIZED NOT NULL)");
            }

            @Override public void onUpgrade(SQLiteDatabase database, int oldVersion, int newVersion) {
                throw new IllegalStateException("Unknown schema version " + oldVersion);
            }
        };
        return true;
    }

    @Override public Uri insert(Uri uri, ContentValues values) {
        return ContentUris.withAppendedId(NOTES, helper.getWritableDatabase().insertOrThrow(TABLE, null, values));
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return helper.getReadableDatabase().query(TABLE, projection, selection, selectionArgs, null, null, sortOrder);
    }

    @Override public String getType(Uri uri) { return "vnd.android.cursor.dir/vnd.stone.note"; }

    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Only creating and reading notes is supported");
    }

    @Override public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Only creating and reading notes is supported");
    }

    @Override public Bundle call(String method, String arg, Bundle extras) {
        if (!"verifyDatabase".equals(method)) return super.call(method, arg, extras);
        SQLiteDatabase database = helper.getWritableDatabase();
        long rowId;
        database.beginTransaction();
        try {
            ContentValues discarded = new ContentValues();
            discarded.put(BODY, "This note must be rolled back");
            rowId = database.insertOrThrow(TABLE, null, discarded);
            require(rowExists(database, rowId), "Uncommitted row is missing inside the transaction");
        } finally {
            database.endTransaction();
        }
        require(!rowExists(database, rowId), "Uncommitted row survived rollback");

        try (Cursor result = database.rawQuery("SELECT upper(?), ? LIKE ?, ? = ? COLLATE LOCALIZED, X'', NULL",
                new String[]{"café", "CAFÉ", "café", "CAFÉ", "cafe\u0301"})) {
            require(result.moveToFirst(), "Unicode query returned no result");
            require("CAFÉ".equals(result.getString(0)), "Unicode upper() returned a different value");
            require(result.getInt(1) == 1, "Unicode LIKE did not match");
            require(result.getInt(2) == 1, "LOCALIZED did not match canonically equivalent text");
            require(result.getBlob(3).length == 0 && result.isNull(4), "Empty BLOB and SQL NULL must differ");
        }

        database.execSQL("CREATE TEMP TABLE IF NOT EXISTS numbers (value REAL)");
        database.execSQL("DELETE FROM numbers");
        try (android.database.sqlite.SQLiteStatement insert = database.compileStatement("INSERT INTO numbers VALUES (?)")) {
            insert.bindDouble(1, 23.5);
            insert.executeInsert();
        }
        try (Cursor result = database.rawQuery("SELECT value, unixepoch('now') FROM numbers", null)) {
            require(result.moveToFirst() && result.getDouble(0) == 23.5, "Floating-point binding changed the value");
            require(Math.abs(result.getLong(1) - System.currentTimeMillis() / 1000) < 60, "SQLite clock differs from Windows");
        }

        // Room can pass a full no-backup path as the SQLiteOpenHelper name.
        File directory = getContext().getNoBackupFilesDir();
        while (directory.getAbsolutePath().length() < 300) {
            directory = new File(directory, "sqlite-long-path-directory");
        }
        require(directory.isDirectory() || directory.mkdirs(), "Could not create long database directory");
        File noBackupDatabase = new File(directory, "work.db").getAbsoluteFile();
        try (SQLiteOpenHelper noBackupHelper = new SQLiteOpenHelper(getContext(), noBackupDatabase.getPath(), null, 1) {
            @Override public void onCreate(SQLiteDatabase db) {
                db.execSQL("CREATE TABLE work (_id INTEGER PRIMARY KEY)");
            }

            @Override public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
                throw new IllegalStateException("Unknown work schema version " + oldVersion);
            }
        }) {
            SQLiteDatabase work = noBackupHelper.getWritableDatabase();
            require(noBackupDatabase.equals(new File(work.getPath())) && noBackupDatabase.isFile(),
                    "Absolute database path was changed");
        }
        android.util.Log.i("WindowsNotes", "SQLite long no-backup path verified");
        return Bundle.EMPTY;
    }

    private static boolean rowExists(SQLiteDatabase database, long rowId) {
        try (Cursor rows = database.rawQuery("SELECT _id FROM notes WHERE _id=?", new String[]{Long.toString(rowId)})) {
            return rows.moveToFirst();
        }
    }

    private static void require(boolean condition, String error) {
        if (!condition) throw new IllegalStateException(error);
    }
}
