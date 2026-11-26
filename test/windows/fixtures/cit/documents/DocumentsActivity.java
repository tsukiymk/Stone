package com.trebledot.stone.cit.documents;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.util.Log;
import android.widget.Button;
import java.io.IOException;
import java.io.InputStream;

public final class DocumentsActivity extends Activity {
    private static final String TAG = "WindowsDocuments";
    private static final int PICK_FOLDER = 1;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Button button = new Button(this);
        button.setText("Select folder");
        button.setOnClickListener(view -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER));
        setContentView(button);
        String saved = getPreferences(MODE_PRIVATE).getString("tree", null);
        if (saved != null) {
            Uri tree = Uri.parse(saved);
            boolean persisted = getContentResolver().getPersistedUriPermissions().stream().anyMatch(
                    permission -> tree.equals(permission.getUri()) && permission.isReadPermission());
            if (!persisted) throw new IllegalStateException("Missing persisted tree grant");
            verifyTree(tree);
            Log.i(TAG, "Persisted tree verified");
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FOLDER) return;
        if (result == RESULT_CANCELED && data == null) {
            Log.i(TAG, "Folder selection cancelled");
            return;
        }
        if (result != RESULT_OK || data == null || data.getData() == null) {
            throw new IllegalStateException("Invalid folder result");
        }
        Uri tree = data.getData();
        getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        verifyTree(tree);
        if (!getPreferences(MODE_PRIVATE).edit().putString("tree", tree.toString()).commit()) {
            throw new IllegalStateException("Could not persist the tree URI");
        }
        Log.i(TAG, "Selected tree verified");
    }

    private void verifyTree(Uri tree) {
        String dir = findChild(tree, DocumentsContract.getTreeDocumentId(tree), "nested");
        String file = findChild(tree, dir, "文件.txt");
        try (InputStream input = getContentResolver().openInputStream(
                DocumentsContract.buildDocumentUriUsingTree(tree, file))) {
            if (input == null || input.read() != 'A' || input.read() != -1) {
                throw new IllegalStateException("Unexpected document contents");
            }
        } catch (IOException error) {
            throw new IllegalStateException("Could not read selected document", error);
        }
    }

    private String findChild(Uri tree, String parent, String name) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
        try (Cursor cursor = getContentResolver().query(children,
                new String[]{Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            if (cursor != null) {
                while (cursor.moveToNext()) {
                    if (name.equals(cursor.getString(1))) return cursor.getString(0);
                }
            }
        }
        throw new IllegalStateException("Missing document " + name + " in " + parent);
    }
}
