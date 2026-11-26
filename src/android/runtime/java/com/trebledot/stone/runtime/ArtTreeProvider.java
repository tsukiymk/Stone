package com.trebledot.stone.runtime;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsProvider;
import java.io.FileNotFoundException;
import java.io.IOException;

/** Directory documents and read descriptors; host paths never enter document IDs. */
public final class ArtTreeProvider extends DocumentsProvider {
    private static final String[] DEFAULT={Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_MIME_TYPE,Document.COLUMN_SIZE,Document.COLUMN_LAST_MODIFIED,Document.COLUMN_FLAGS};
    @Override public boolean onCreate() { return true; }
    @Override public Cursor queryRoots(String[] projection) { return new MatrixCursor(projection==null?new String[]{"root_id"}:projection); }
    @Override public Cursor queryDocument(String id,String[] projection) throws FileNotFoundException {
        try {
            String[] data=ArtDocumentTrees.statNative(id);
            if(Boolean.getBoolean("stone.diagnostics")) android.util.Log.i("StoneDocuments","queryDocument id="+id+" name="+data[1]);
            return rows(data,projection);
        } catch(IOException error) { android.util.Log.w("StoneDocuments","Directory metadata query failed",error); throw missing(error); }
    }
    @Override public Cursor queryChildDocuments(String id,String[] projection,String order) throws FileNotFoundException {
        if(order!=null && !order.isEmpty()) throw new UnsupportedOperationException("Directory sort orders are unsupported");
        try { return rows(ArtDocumentTrees.listNative(id),projection); } catch(IOException error) { throw missing(error); }
    }
    @Override public boolean isChildDocument(String parent,String child) {
        if(!child.startsWith(parent+"/")) return false;
        try { ArtDocumentTrees.statNative(parent); ArtDocumentTrees.statNative(child); return true; }
        catch(IOException invalid) { return false; }
    }
    @Override public ParcelFileDescriptor openDocument(String id,String mode,CancellationSignal cancellation) throws FileNotFoundException {
        if(cancellation!=null) cancellation.throwIfCanceled();
        if(!"r".equals(mode)) throw new FileNotFoundException("Selected directories provide read-only access");
        try { return ParcelFileDescriptor.adoptFd(ArtDocumentTrees.openNative(id)); } catch(IOException error) { throw missing(error); }
    }
    private static FileNotFoundException missing(IOException error) {
        FileNotFoundException failure=new FileNotFoundException("Cannot access selected directory document"); failure.initCause(error); return failure;
    }
    private static Cursor rows(String[] data,String[] projection) {
        String[] columns=projection==null?DEFAULT:projection; MatrixCursor cursor=new MatrixCursor(columns);
        for(int row=0;row<data.length;row+=5) {
            Object[] values=new Object[columns.length]; boolean directory="1".equals(data[row+2]);
            for(int col=0;col<columns.length;++col) switch(columns[col]) {
                case Document.COLUMN_DOCUMENT_ID: values[col]=data[row]; break;
                case Document.COLUMN_DISPLAY_NAME: values[col]=data[row+1]; break;
                case Document.COLUMN_MIME_TYPE: values[col]=directory?Document.MIME_TYPE_DIR:"application/octet-stream"; break;
                case Document.COLUMN_SIZE: values[col]=directory?null:Long.valueOf(data[row+3]); break;
                case Document.COLUMN_LAST_MODIFIED: values[col]=Long.valueOf(data[row+4]); break;
                case Document.COLUMN_FLAGS: values[col]=0; break;
                default: cursor.close(); throw new IllegalArgumentException("Unknown directory document column: "+columns[col]);
            }
            cursor.addRow(values);
        }
        return cursor;
    }
}
