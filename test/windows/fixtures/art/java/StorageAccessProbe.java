package com.trebledot.stone.probe;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Switch;
import com.trebledot.stone.runtime.ArtApplication;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;

/** Synthetic CIT permission UI and real mounted filesystem behavior. No APK policy is pre-seeded. */
final class StorageAccessProbe {
    private static Activity current(ArtApplication app) throws Exception {
        Method method=ArtApplication.class.getDeclaredMethod("currentActivity",Activity.class); method.setAccessible(true);
        return (Activity)method.invoke(app,new Object[]{null});
    }
    private static void check(boolean value,String message) { if (!value) throw new AssertionError(message); }
    private static void revokeTestGrant() throws Exception {
        java.lang.reflect.Field field=Class.forName("com.trebledot.stone.runtime.ArtServices").getDeclaredField("storage"); field.setAccessible(true);
        Object storage=field.get(null); Method revoke=storage.getClass().getDeclaredMethod("setAllFiles",boolean.class); revoke.setAccessible(true);
        revoke.invoke(storage,false);
    }
    private static void denied(File file) throws Exception {
        try (FileOutputStream stream=new FileOutputStream(file)) { throw new AssertionError("Unowned shared-storage file opened: "+file); }
        catch (java.io.FileNotFoundException expected) { }
    }
    static void verify(ArtApplication app) throws Exception {
        String name=app.context.getPackageName();
        Intent request=new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).setData(Uri.parse("package:"+name));
        ActivityInfo target=app.context.getPackageManager().resolveActivity(request,0).activityInfo;
        check(!target.exported && name.equals(target.packageName),"private storage access target");
        for (Intent bad:new Intent[]{new Intent(request).setData(Uri.parse("package:other.app")),new Intent(request).putExtra("allowed",true),
                new Intent(request).setData(Uri.parse("file:///sdcard")),new Intent(request).setPackage("other.app")})
            check(app.context.getPackageManager().resolveActivity(bad,0)==null,"forged storage permission request");
        request.setComponent(new android.content.ComponentName(target.packageName,target.name));
        check(!Environment.isExternalStorageManager(),"fresh all-files grant");
        revokeTestGrant();
        File firstRun=new File(app.context.getCacheDir(),"storage-lazy-checked");
        if (!firstRun.exists()) {
            check(!new File("/storage/emulated/0/Android/data/"+name).exists(),"startup must not pre-create application private storage");
            check(firstRun.createNewFile(),"lazy storage verification marker");
        }
        File own=app.context.getExternalFilesDir(null);
        check(own!=null,"own external files directory");
        try(FileOutputStream out=new FileOutputStream(new File(own,"own.txt"))) { out.write(1); }
        File cache=app.context.getExternalCacheDir();
        check(cache!=null && cache.isDirectory(),"own external cache without all-files grant");
        try(FileOutputStream out=new FileOutputStream(new File(cache,"cache.txt"))) { out.write(2); }
        File shared=new File("/sdcard/stone-storage-probe.txt"); denied(shared);
        Method launch=ArtApplication.class.getDeclaredMethod("launch",ActivityInfo.class,Intent.class,android.os.IBinder.class,String.class,int.class);
        launch.setAccessible(true);
        // This synthetic CIT declaration is restored after testing. The real APK
        // remains unmodified and requires its user's settings toggle.
        String[] permissions=app.apk.packageInfo.requestedPermissions;
        java.util.ArrayList<String> declared=new java.util.ArrayList<>(permissions==null ? java.util.Collections.emptyList() : java.util.Arrays.asList(permissions));
        declared.add("android.permission.MANAGE_EXTERNAL_STORAGE");
        app.apk.packageInfo.requestedPermissions=declared.toArray(new String[0]);
        Activity screen=null;
        try {
            launch.invoke(app,target,request,null,null,-1); screen=current(app);
            check(screen.getApplication()!=app.application,"system page must not dispatch APK Application callbacks");
            screen.onBackPressed(); BroadcastProbe.drain();
            check(!Environment.isExternalStorageManager(),"Back must preserve denial");
            launch.invoke(app,target,request,null,null,-1); screen=current(app);
            ((Switch)screen.findViewById(android.R.id.checkbox)).performClick(); waitFor(true);
            check(Environment.getExternalStorageDirectory().getPath().equals("/storage/emulated/0"),"primary emulated mount point");
            for(String directory:new String[]{"Android","Movies","Documents","Download","Music","Pictures","DCIM","Alarms","Audiobooks","Notifications","Podcasts","Recordings","Ringtones"})
                check(new File(Environment.getExternalStorageDirectory(),directory).isDirectory(),"standard public directory "+directory);
            try(FileOutputStream out=new FileOutputStream(shared)) { out.write(42); }
            for(String alias:new String[]{"/sdcard","/data/media/0","/storage/emulated/0","/storage/self/primary"})
                try(java.io.FileInputStream input=new java.io.FileInputStream(new File(alias,shared.getName()))) {
                    check(input.read()==42 && input.read()==-1,"shared-storage alias bytes");
                }
            for(String privatePath:new String[]{"/sdcard/Android/data/foreign.pkg/no.txt","/sdcard/android/DATA/foreign.pkg/no.txt","/sdcard/Android/obb/foreign.pkg/no.txt"})
                denied(new File(privatePath));
            File secret=new File(app.context.getCacheDir(),"storage-secret.txt");
            try(FileOutputStream out=new FileOutputStream(secret)) { out.write(3); }
            File escape=new File("/sdcard/stone-storage-escape");
            android.system.Os.symlink(secret.getPath(),escape.getPath());
            try { denied(escape); } finally { check(escape.delete(),"remove storage escape link"); }
            screen.recreate(); BroadcastProbe.drain(); screen=current(app);
            check(((Switch)screen.findViewById(android.R.id.checkbox)).isChecked(),"grant survives settings recreation");
            ((Switch)screen.findViewById(android.R.id.checkbox)).performClick(); waitFor(false);
            denied(shared);
            System.out.println("PASS shared-storage settings: cancel, grant, revoke, AppOps cache, recreation, own files, four aliases and private/symlink boundary");
        } finally {
            if (screen!=null && !screen.isFinishing()) { screen.onBackPressed(); BroadcastProbe.drain(); }
            revokeTestGrant();
            app.apk.packageInfo.requestedPermissions=permissions;
        }
    }
    private static void waitFor(boolean granted) throws Exception {
        long deadline=android.os.SystemClock.uptimeMillis()+10000;
        while(Environment.isExternalStorageManager()!=granted && android.os.SystemClock.uptimeMillis()<deadline) {
            Thread.sleep(10); BroadcastProbe.drain();
        }
        check(Environment.isExternalStorageManager()==granted,"storage grant callback/cache");
        BroadcastProbe.drain();
    }
}
