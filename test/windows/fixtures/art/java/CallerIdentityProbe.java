package com.trebledot.stone.probe;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.IBinder;
import com.trebledot.stone.runtime.ArtApplication;
import java.lang.reflect.Method;

/** Trusted local task setup; assertions use the image Activity caller/result APIs. */
final class CallerIdentityProbe {
    private static Caller caller;
    private static Child child;
    private static int results;
    private static int expectedCode = Activity.RESULT_OK, backFinishes;
    private static String expectedValue = "received";
    public static final class Caller extends Activity {
        @Override protected void onCreate(Bundle saved) { super.onCreate(saved); caller = this; }
        @Override protected void onActivityResult(int request, int result, Intent data) {
            super.onActivityResult(request,result,data);
            if (request != 42 || result != expectedCode || (expectedValue == null ? data != null
                    : data == null || !expectedValue.equals(data.getStringExtra("value"))))
                throw new AssertionError("Local result lost caller routing");
            results++;
        }
    }
    public static final class Child extends Activity {
        @Override protected void onCreate(Bundle saved) { super.onCreate(saved); child = this; }
        @Override public void finishAfterTransition() { backFinishes++; super.finishAfterTransition(); }
    }
    private static IBinder token(Activity activity) throws Exception {
        java.lang.reflect.Field field = Activity.class.getDeclaredField("mToken"); field.setAccessible(true); return (IBinder) field.get(activity);
    }
    private static void check(Activity target, ComponentName expected) {
        if (!java.util.Objects.equals(expected,target.getCallingActivity())
                || !java.util.Objects.equals(expected == null ? null : expected.getPackageName(),target.getCallingPackage()))
            throw new AssertionError("Image Activity caller identity differs from the trusted task record");
    }
    static void run(ArtApplication application) throws Exception {
        results = 0;
        Method launch = ArtApplication.class.getDeclaredMethod("launch",ActivityInfo.class,Intent.class,IBinder.class,String.class,int.class);
        launch.setAccessible(true);
        ActivityInfo parent = new ActivityInfo(application.apk.activityInfo);
        parent.name = application.context.getPackageName()+".StoneCallerAlias"; parent.targetActivity = Caller.class.getName();
        Intent parentIntent = new Intent().setComponent(new ComponentName(parent.packageName,parent.name));
        launch.invoke(application,parent,parentIntent,null,null,-1);
        try {
            ComponentName identity = caller.getComponentName();
            if (!identity.equals(parentIntent.getComponent())) throw new AssertionError("Caller alias identity lost");
            check(caller,null);
            check(new Activity(),null);
            ActivityInfo target = new ActivityInfo(parent); target.name = Child.class.getName(); target.targetActivity = null;
            Intent request = new Intent().setComponent(new ComponentName(target.packageName,target.name))
                .putExtra(Intent.EXTRA_PACKAGE_NAME,"com.trebledot.stone.spoofed");
            launch.invoke(application,target,request,token(caller),null,42);
            check(child,identity);
            caller.setIntent(new Intent().setComponent(new ComponentName("com.trebledot.stone.spoofed","FakeCaller")));
            check(child,identity);
            Child previousChild = child; child.recreate(); BroadcastProbe.drain();
            if (child == previousChild) throw new AssertionError("Result target did not recreate");
            check(child,identity);
            Caller previousCaller = caller; caller.recreate(); BroadcastProbe.drain();
            if (caller == previousCaller) throw new AssertionError("Stopped result caller did not recreate");
            check(caller,null); check(child,identity);
            Child finished = child;
            child.setResult(Activity.RESULT_OK,new Intent().putExtra("value","received")); child.finish(); BroadcastProbe.drain();
            if (results != 1) throw new AssertionError("Recreated caller did not receive exactly one result");
            check(finished,null);
            launch.invoke(application,target,request,token(caller),null,-1);
            check(child,null);
            child.finish(); BroadcastProbe.drain();
            if (results != 1) throw new AssertionError("Ordinary launch delivered a result");
            for (int code : new int[]{Activity.RESULT_CANCELED,Activity.RESULT_OK,Activity.RESULT_CANCELED,73}) {
                int before = results;
                expectedCode = code; expectedValue = before == 1 ? null : "back-"+before;
                launch.invoke(application,target,request,token(caller),null,42);
                if (expectedValue != null) child.setResult(code,new Intent().putExtra("value",expectedValue));
                backFinishes = 0;
                child.onBackPressed();
                long deadline = android.os.SystemClock.uptimeMillis()+15000;
                while (results == before && android.os.SystemClock.uptimeMillis() < deadline) BroadcastProbe.drain();
                if (results != before+1 || backFinishes != 1 || !child.isFinishing())
                    throw new AssertionError("Back bypassed framework finish or lost its exact saved result");
                check(child,null); BroadcastProbe.drain();
                if (results != before+1) throw new AssertionError("Back duplicated its Activity result");
            }
            System.out.println("PASS image Activity caller package/component: alias, no-result null, spoof isolation, caller/target recreation and result delivery.");
            System.out.println("PASS image Activity Back: framework finishAfterTransition, finishing state, default cancellation, saved OK/cancel/custom data and one result each.");
        } finally { application.destroyActivity(); }
    }
}
