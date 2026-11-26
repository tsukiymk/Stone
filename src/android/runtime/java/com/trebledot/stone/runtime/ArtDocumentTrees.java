package com.trebledot.stone.runtime;

import android.content.Intent;
import android.content.UriPermission;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.provider.DocumentsContract;
import java.io.IOException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.LinkedHashMap;

/** Read-only Windows directories explicitly selected by the current application. */
final class ArtDocumentTrees {
    static final String AUTHORITY="com.trebledot.stone.trees";
    private static native String[] initializeNative(String packageName) throws IOException;
    static native long createNative() throws IOException;
    static native String[] pickNative(long request) throws IOException;
    static native void discardNative(String root);
    private static native void persistNative(String root,long time) throws IOException;
    static native String[] statNative(String document) throws IOException;
    static native String[] listNative(String document) throws IOException;
    static native int openNative(String document) throws IOException;
    private static final LinkedHashMap<String,Long> grants=new LinkedHashMap<>();
    private static String packageName;
    private static int uid,user;
    static synchronized void initialize(ArtPackage apk,int localUser) throws IOException {
        if(packageName!=null) throw new IllegalStateException("Document trees already initialized");
        uid=apk.applicationInfo.uid; user=localUser; packageName=apk.applicationInfo.packageName;
        String[] saved=initializeNative(packageName);
        for(int i=0;i<saved.length;i+=3) grants.put(saved[i],Long.parseLong(saved[i+2]));
    }
    static synchronized Uri publish(String id) {
        if(!id.matches("[0-9A-Fa-f-]{36}")) throw new IllegalArgumentException("Invalid selected directory identifier");
        grants.put(id,0L); return DocumentsContract.buildTreeDocumentUri(AUTHORITY,id);
    }
    private static String root(Uri uri) {
        if(uri==null || !"content".equals(uri.getScheme()) || !AUTHORITY.equals(uri.getAuthority())
            || uri.getQuery()!=null || uri.getFragment()!=null || uri.getPathSegments().size()!=2
            || !"tree".equals(uri.getPathSegments().get(0))) throw new SecurityException("Unknown document tree URI");
        String root=DocumentsContract.getTreeDocumentId(uri);
        if(!grants.containsKey(root)) throw new SecurityException("Directory was not selected by this application");
        return root;
    }
    static synchronized Object permissions(Object instance,Method method,Object[] args) throws Exception {
        if(Binder.getCallingUid()!=uid) throw new SecurityException("Foreign document grant caller");
        switch(method.getName()) {
            case "takePersistableUriPermission":
            case "releasePersistableUriPermission": {
                if((Integer)args[1]!=Intent.FLAG_GRANT_READ_URI_PERMISSION || (Integer)args[3]!=user
                    || args[2]!=null && !packageName.equals(args[2])) throw new SecurityException("Unsupported document grant scope");
                String id=root((Uri)args[0]);
                long time=method.getName().startsWith("take")?System.currentTimeMillis():0;
                persistNative(id,time); grants.put(id,time); return null;
            }
            case "getUriPermissions": {
                if(!packageName.equals(args[0])) throw new SecurityException("Foreign package document grants");
                ArrayList<UriPermission> permissions=new ArrayList<>();
                if((Boolean)args[1]) for(java.util.Map.Entry<String,Long> grant:grants.entrySet()) {
                    if((Boolean)args[2] && grant.getValue()==0) continue;
                    Uri uri=DocumentsContract.buildTreeDocumentUri(AUTHORITY,grant.getKey());
                    permissions.add(UriPermission.class.getConstructor(Uri.class,int.class,long.class).newInstance(uri,1,
                        grant.getValue()==0?UriPermission.INVALID_TIME:grant.getValue()));
                }
                return Class.forName("android.content.pm.ParceledListSlice").getConstructor(java.util.List.class).newInstance(permissions);
            }
            default: throw new UnsupportedOperationException("ART URI grants: "+method.getName());
        }
    }
    static boolean accepts(String caller,Intent intent) {
        if(intent==null || !Intent.ACTION_OPEN_DOCUMENT_TREE.equals(intent.getAction()) || intent.getData()!=null
            || intent.getType()!=null || intent.getSelector()!=null || intent.getClipData()!=null
            || intent.getPackage()!=null && !caller.equals(intent.getPackage())) return false;
        if(intent.getComponent()!=null && (!caller.equals(intent.getComponent().getPackageName())
            || !ArtDocumentActivity.class.getName().equals(intent.getComponent().getClassName()))) return false;
        if(intent.getCategories()!=null) for(String category:intent.getCategories())
            if(!Intent.CATEGORY_DEFAULT.equals(category)) return false;
        Bundle extras=intent.getExtras();
        if(extras!=null) for(String key:extras.keySet())
            if(!Intent.EXTRA_LOCAL_ONLY.equals(key) || !(extras.get(key) instanceof Boolean)) return false;
        return true;
    }
}
