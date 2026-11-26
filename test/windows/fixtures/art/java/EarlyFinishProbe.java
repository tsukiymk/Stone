package com.trebledot.stone.probe;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.IBinder;
import com.trebledot.stone.runtime.ArtApplication;
import java.lang.reflect.Method;

/** Real framework callbacks for finish() before resume and during result dispatch. */
final class EarlyFinishProbe {
    private static Parent parent;
    private static Child child;
    private static int results, childStarts, childResumes, childDestroys, parentResumes, parentDestroys;
    private static boolean finishParent;
    public static final class Parent extends Activity {
        @Override protected void onCreate(Bundle saved) { super.onCreate(saved); parent=this; }
        @Override protected void onResume() {
            if (isFinishing()) throw new AssertionError("Result callback finished the parent before resume");
            super.onResume(); parentResumes++;
        }
        @Override protected void onDestroy() { super.onDestroy(); parentDestroys++; }
        @Override protected void onActivityResult(int request,int result,Intent data) {
            super.onActivityResult(request,result,data);
            if (request!=63 || result!=RESULT_OK || data==null || !"early-finish".equals(data.getStringExtra("value")))
                throw new AssertionError("Early finish lost its real Activity result");
            results++; if (finishParent) finish();
        }
    }
    public static final class Child extends Activity {
        private void complete() { setResult(RESULT_OK,new Intent().putExtra("value","early-finish")); finish(); }
        @Override protected void onCreate(Bundle saved) {
            super.onCreate(saved); child=this;
            String at=getIntent().getStringExtra("at");
            if ("create".equals(at) || ("recreate".equals(at) && saved!=null)) complete();
        }
        @Override protected void onStart() {
            if (isFinishing()) throw new AssertionError("Finishing Activity received onStart");
            super.onStart(); childStarts++;
            if ("start".equals(getIntent().getStringExtra("at"))) complete();
        }
        @Override protected void onResume() {
            if (isFinishing()) throw new AssertionError("Finishing Activity received onResume");
            super.onResume(); childResumes++;
        }
        @Override protected void onDestroy() { super.onDestroy(); childDestroys++; }
    }
    private static IBinder token(Activity activity) throws Exception {
        java.lang.reflect.Field field=Activity.class.getDeclaredField("mToken"); field.setAccessible(true); return (IBinder)field.get(activity);
    }
    private static ActivityInfo info(ArtApplication app,Class<?> type) {
        ActivityInfo result=new ActivityInfo(app.apk.activityInfo); result.name=type.getName(); result.targetActivity=null; return result;
    }
    private static Intent intent(ActivityInfo info) { return new Intent().setComponent(new ComponentName(info.packageName,info.name)); }
    static void run(ArtApplication app) throws Exception {
        Method launch=ArtApplication.class.getDeclaredMethod("launch",ActivityInfo.class,Intent.class,IBinder.class,String.class,int.class);
        launch.setAccessible(true);
        for (String at:new String[]{"create","start","recreate","result"}) {
            results=childStarts=childResumes=childDestroys=parentResumes=parentDestroys=0; finishParent=at.equals("result");
            ActivityInfo owner=info(app,Parent.class), target=info(app,Child.class);
            launch.invoke(app,owner,intent(owner),null,null,-1);
            try {
                launch.invoke(app,target,intent(target).putExtra("at",finishParent ? "create" : at),token(parent),null,63);
                BroadcastProbe.drain();
                if (at.equals("recreate")) {
                    if (results!=0 || childStarts!=1 || childResumes!=1) throw new AssertionError("Recreation setup lost the live child");
                    Child previous=child; child.recreate(); BroadcastProbe.drain();
                    if (child==previous) throw new AssertionError("Early-finish target did not recreate");
                }
                int expectedStarts=at.equals("start") || at.equals("recreate") ? 1 : 0;
                int expectedResumes=at.equals("recreate") ? 1 : 0;
                // Relaunch queues onCreate's finish behind the current drain marker;
                // result callbacks can likewise queue the parent's own finish.
                long deadline=android.os.SystemClock.uptimeMillis()+15000;
                while ((results!=1 || childDestroys!=(at.equals("recreate") ? 2 : 1)
                        || (finishParent && parentDestroys!=1)) && android.os.SystemClock.uptimeMillis()<deadline)
                    BroadcastProbe.drain();
                if (results!=1 || childStarts!=expectedStarts || childResumes!=expectedResumes
                        || childDestroys!=(at.equals("recreate") ? 2 : 1) || parentResumes!=(finishParent ? 1 : 2)
                        || parentDestroys!=(finishParent ? 1 : 0))
                    throw new AssertionError("Invalid early-finish lifecycle/results for "+at+": "+results+","+childStarts+","+childResumes+","+childDestroys+","+parentResumes+","+parentDestroys);
                BroadcastProbe.drain();
                if (results!=1) throw new AssertionError("Early finish duplicated its result");
            } finally { app.destroyActivity(); }
        }
        windowResult(app,launch);
        System.out.println("PASS image Activity early finish: create/start, recreated create, parent result finish, skipped resume, one result and exact destruction.");
    }
    private static void windowResult(ArtApplication app,Method launch) throws Exception {
        app.startActivity(); finishParent=true; results=0;
        ActivityInfo owner=info(app,Parent.class),target=info(app,Child.class);
        launch.invoke(app,owner,intent(owner),null,null,-1);
        launch.invoke(app,target,intent(target).putExtra("at","window-result"),token(parent),null,63);
        Class<?> type=Class.forName("com.trebledot.stone.runtime.ArtWindowLoop");
        java.lang.reflect.Constructor<?> constructor=type.getDeclaredConstructor(Activity.class,int.class,int.class); constructor.setAccessible(true);
        Method update=type.getDeclaredMethod("updateActivity"),close=type.getDeclaredMethod("close"); update.setAccessible(true);close.setAccessible(true);
        Object loop=constructor.newInstance(child,360,640);
        try {
            Method finish=ArtApplication.class.getDeclaredMethod("finish",IBinder.class,int.class,Intent.class);finish.setAccessible(true);
            finish.invoke(app,token(child),Activity.RESULT_OK,new Intent().putExtra("value","early-finish"));
            if(results!=1 || !parent.isFinishing() || child.getWindow().getDecorView().isAttachedToWindow()
                || !Boolean.FALSE.equals(update.invoke(loop))) throw new AssertionError("Finishing result caller became a software input target");
            BroadcastProbe.drain();
            if(!Boolean.TRUE.equals(update.invoke(loop))) throw new AssertionError("Software window did not recover its live predecessor");
        } finally {close.invoke(loop);app.destroyActivity();finishParent=false;}
        System.out.println("PASS software window defers detached input through a cascading result finish and attaches the surviving Activity");
    }
}
