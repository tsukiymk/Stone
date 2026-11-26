package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/** Keeps the originating Android result pending while the Windows save dialog is open. */
public final class ArtDocumentActivity extends Activity {
    private Operation operation;
    @Override protected void onCreate(Bundle state) {
        ArtSettingsUi.prepare(this);
        super.onCreate(state);
        if (!ArtDocuments.accepts(getPackageName(),getIntent())) { finish(); return; }
        Object retained = getLastNonConfigurationInstance();
        operation = retained instanceof Operation ? (Operation) retained : new Operation(getIntent());
        operation.owner = this; render(); operation.deliver();
    }
    private void render() {
        ArtSettingsUi ui = new ArtSettingsUi(this,operation.tree ? ArtStrings.document_tree_title : ArtStrings.document_save_title);
        if (operation.error) ui.message(ArtText.get(this,ArtStrings.document_error));
        else ui.waiting(ArtText.get(this,operation.tree ? ArtStrings.document_tree_wait : ArtStrings.document_save_wait));
        ui.show();
    }
    @Override public void onConfigurationChanged(Configuration config) { super.onConfigurationChanged(config); render(); }
    @Override public Object onRetainNonConfigurationInstance() { return operation; }
    @Override protected void onDestroy() {
        if (operation != null && operation.owner == this) {
            operation.owner = null; if (!isChangingConfigurations()) operation.cancel();
        }
        super.onDestroy();
    }
    private static final class Operation {
        final Handler main = new Handler(Looper.getMainLooper());
        final String mime;
        final boolean tree;
        ArtDocumentActivity owner;
        volatile long request;
        volatile boolean closed;
        String[] selection;
        boolean done, error;
        Operation(Intent intent) {
            mime = intent.getType();
            tree = Intent.ACTION_OPEN_DOCUMENT_TREE.equals(intent.getAction());
            try {
                request = tree ? ArtDocumentTrees.createNative() : ArtDocuments.createNative(ArtDocuments.filename(intent),mime);
                Thread worker = new Thread(() -> {
                    String[] picked = null; boolean failed = false;
                    long id = request;
                    try { picked = tree ? ArtDocumentTrees.pickNative(id) : ArtDocuments.pickNative(id); }
                    catch (java.io.IOException | RuntimeException | LinkageError failure) {
                        android.util.Log.w("StoneDocuments","Cannot select a Windows document",failure); failed = true;
                    } finally { request = 0; ArtDocuments.releaseNative(id); }
                    final String[] result = picked; final boolean invalid = failed;
                    main.post(() -> { selection = result; error = invalid; done = true; deliver(); });
                },"Stone document selection");
                worker.setDaemon(true);
                try { worker.start(); } catch (RuntimeException | Error failure) { ArtDocuments.releaseNative(request); request = 0; throw failure; }
            } catch (java.io.IOException | RuntimeException | LinkageError failure) {
                android.util.Log.w("StoneDocuments","Cannot open a Windows save dialog",failure); error = true;
            }
        }
        void cancel() {
            closed = true;
            long id = request;
            if (id != 0) try { ArtDocuments.cancelNative(id); } catch (java.io.IOException ignored) { /* A completed selection already released its request. */ }
            deliver();
        }
        void deliver() {
            if (!done) return;
            if (closed) { if (selection != null) {
                if(tree) ArtDocumentTrees.discardNative(selection[0]); else ArtDocuments.releaseGrantNative(Long.parseLong(selection[0]));
                selection = null; } return; }
            if (owner == null) return;
            if (error) { owner.render(); return; }
            if (selection != null) {
                android.net.Uri uri = tree ? ArtDocumentTrees.publish(selection[0]) : ArtDocuments.publish(Long.parseLong(selection[0]),selection[1],mime); selection = null;
                owner.setResult(RESULT_OK,new Intent().setData(uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | (tree ? Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION : Intent.FLAG_GRANT_WRITE_URI_PERMISSION)));
            }
            owner.finish();
        }
    }
}
