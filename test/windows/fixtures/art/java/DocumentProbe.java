package com.trebledot.stone.probe;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

/** Public resolver and content-URI boundaries; no picker approval is injected. */
final class DocumentProbe {
    static void verify(Context context) throws Exception {
        java.io.File syncFile=new java.io.File(context.getCacheDir(),"descriptor-sync.bin");
        try(java.io.FileOutputStream output=new java.io.FileOutputStream(syncFile)) {
            output.write(new byte[]{1,2,3}); output.getFD().sync();
        }
        try { new java.io.FileDescriptor().sync(); throw new AssertionError("Unopened descriptor sync succeeded"); }
        catch(java.io.SyncFailedException expected) {}
        android.util.AtomicFile atomic=new android.util.AtomicFile(new java.io.File(context.getCacheDir(),"atomic-closed-stream.bin"));
        java.io.FileOutputStream closed=atomic.startWrite(); closed.write(42); closed.close();
        atomic.finishWrite(closed); // The original APK also closes before finishWrite.
        try(java.io.FileInputStream input=atomic.openRead()) {
            if(input.read()!=42 || input.read()!=-1) throw new AssertionError("Closed AtomicFile stream was not published");
        }
        Intent create = new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain")
            .addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE,"document-test.txt");
        android.content.pm.ResolveInfo resolved = context.getPackageManager().resolveActivity(create,0);
        if (resolved == null || !context.getPackageName().equals(resolved.activityInfo.packageName) || resolved.activityInfo.exported)
            throw new AssertionError("Document creation does not resolve to a private current-app Activity");
        Intent clipped = new Intent(create); clipped.setClipData(android.content.ClipData.newPlainText("test","payload"));
        for (Intent bad : new Intent[]{
                new Intent(create).setData(Uri.parse("file:///data/document-test.txt")),
                new Intent(create).setType("text/*"),
                new Intent(create).setPackage("com.android.documentsui"),
                new Intent(create).putExtra("android.intent.extra.USER_ID",10),
                new Intent(create).addCategory(Intent.CATEGORY_BROWSABLE),
                clipped}) {
            if (context.getPackageManager().resolveActivity(bad,0) != null)
                throw new AssertionError("Document resolver accepted unsupported data, target or payload: " + bad);
        }
        for (String value : new String[]{
                "content://com.trebledot.stone.documents/document/not-selected",
                "content://com.trebledot.stone.documents/document/C%3A%5CWindows%5Cwin.ini",
                "content://com.trebledot.stone.documents/document/../settings?path=/data/document-test.txt"}) {
            try (ParcelFileDescriptor descriptor = context.getContentResolver().openFileDescriptor(Uri.parse(value),"w")) {
                throw new AssertionError("Unselected document URI opened a file: " + descriptor);
            } catch (java.io.FileNotFoundException expected) { }
        }
        Intent tree=new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).putExtra(Intent.EXTRA_LOCAL_ONLY,true);
        android.content.pm.ResolveInfo treeTarget=context.getPackageManager().resolveActivity(tree,0);
        if(treeTarget==null || !context.getPackageName().equals(treeTarget.activityInfo.packageName)) throw new AssertionError("Tree picker resolver");
        Intent routed=new Intent(tree).setComponent(new android.content.ComponentName(context.getPackageName(),treeTarget.activityInfo.name));
        if(context.getPackageManager().resolveActivity(routed,0)==null) throw new AssertionError("Routed tree picker rejected");
        for(Intent bad:new Intent[]{new Intent(tree).setData(Uri.parse("file:///data")),new Intent(tree).setPackage("other.app"),
                new Intent(tree).putExtra("hostPath","C:\\Windows"),new Intent(tree).setType("*/*")})
            if(context.getPackageManager().resolveActivity(bad,0)!=null) throw new AssertionError("Unsafe tree picker payload accepted");
        if(!context.getContentResolver().getPersistedUriPermissions().isEmpty()) throw new AssertionError("Fresh application inherited directory grants");
        Uri unknown=android.provider.DocumentsContract.buildTreeDocumentUri("com.trebledot.stone.trees","00000000-0000-0000-0000-000000000000");
        try { context.getContentResolver().takePersistableUriPermission(unknown,1); throw new AssertionError("Unselected tree permission persisted"); }
        catch(SecurityException expected) {}
        Uri child=android.provider.DocumentsContract.buildDocumentUriUsingTree(unknown,"00000000-0000-0000-0000-000000000000/rom.gba");
        if(!android.provider.DocumentsContract.isDocumentUri(context,child)
            || android.provider.DocumentsContract.isDocumentUri(context,unknown)
            || android.provider.DocumentsContract.isDocumentUri(context,Uri.parse("content://foreign.provider/document/unknown")))
            throw new AssertionError("DocumentsProvider discovery does not identify tree documents correctly");
        try(ParcelFileDescriptor descriptor=context.getContentResolver().openFileDescriptor(child,"r")) {
            throw new AssertionError("Unselected tree document opened: "+descriptor);
        } catch(java.io.FileNotFoundException | SecurityException expected) {}
        try {
            Process process = Runtime.getRuntime().exec(new String[]{"stone-unsupported-executable"});
            process.destroy(); throw new AssertionError("Unsupported executable was started");
        } catch (java.io.IOException expected) { /* Missing execution backend is recoverable, never a class-init InternalError. */ }
        System.out.println("PASS document resolver isolation, forged URI rejection and recoverable unsupported process launch; no native picker opened.");
    }
}
