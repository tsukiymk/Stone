package com.trebledot.stone.runtime;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.FileNotFoundException;
import java.io.IOException;

/** Descriptor access only: URI text never names a Windows filesystem path. */
public final class ArtDocumentProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    @Override public String getType(Uri uri) { return ArtDocuments.require(uri).mime; }
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        ArtDocuments.Grant grant = ArtDocuments.require(uri);
        if (selection != null || args != null || order != null) throw new IllegalArgumentException("Document query filters are unsupported");
        String[] columns = projection == null ? new String[]{OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE} : projection;
        MatrixCursor cursor = new MatrixCursor(columns,1); Object[] row = new Object[columns.length];
        for (int index = 0; index < columns.length; index++) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[index])) row[index] = grant.name;
            else if (OpenableColumns.SIZE.equals(columns[index])) try { row[index] = grant.size(); }
            catch (IOException error) { cursor.close(); throw new IllegalStateException("Cannot read selected document size",error); }
            else { cursor.close(); throw new IllegalArgumentException("Unknown document column"); }
        }
        cursor.addRow(row); return cursor;
    }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        try { return ArtDocuments.require(uri).open(mode); }
        catch (IOException | IllegalArgumentException error) {
            FileNotFoundException missing = new FileNotFoundException("Cannot open selected document"); missing.initCause(error); throw missing;
        }
    }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("Select a document through ACTION_CREATE_DOCUMENT"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException("Document metadata changes are unsupported"); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException("Document deletion is unsupported"); }
}
