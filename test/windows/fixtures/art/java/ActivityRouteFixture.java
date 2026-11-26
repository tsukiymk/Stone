package com.trebledot.stone.probe;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/** Two real APK processes; the reflective store call only prepares this signed test target. */
public final class ActivityRouteFixture {
    private static final String SOURCE = "com.trebledot.stone.route.source", TARGET = "com.trebledot.stone.route.target";
    private static final ComponentName ALIAS = new ComponentName(SOURCE,SOURCE+".CallerAlias");
    private static final ComponentName RECEIVER = new ComponentName(TARGET,Target.class.getName());
    private static final String[] MODES = {"echo","cancel","ordinary","die","echo-again","caller-gone","service-restart","back"};
    private static void event(Activity activity, String value) {
        try {
            File directory = activity.getFilesDir(); directory.mkdirs();
            try (FileOutputStream out = new FileOutputStream(new File(directory,"events.txt"),true)) {
                out.write((value+"\n").getBytes(StandardCharsets.UTF_8)); out.getFD().sync();
            }
            android.util.Log.i("StoneRouteFixture",value);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void ensure(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static void notificationHost(Activity activity) {
        try {
            Class<?> type=Class.forName("com.trebledot.stone.runtime.ArtNotifications");
            java.lang.reflect.Method init=type.getDeclaredMethod("initializeNative",String.class); init.setAccessible(true);
            ensure(Boolean.TRUE.equals(init.invoke(null,activity.getPackageName())),"Notification host did not initialize");
            event(activity,"private-notification-host-"+android.os.Process.myPid());
        } catch(Exception error) { throw new AssertionError("Shared notification host initialization failed",error); }
    }
    private static void installTarget(Activity activity) throws Exception {
        String marker = new String(java.nio.file.Files.readAllBytes(new File("/data/stone-route-fixture.txt").toPath()),StandardCharsets.UTF_8);
        ensure(marker.equals("Stone disposable Activity route fixture\n"),"Not the explicit route fixture profile");
        Class<?> storeType = Class.forName("com.trebledot.stone.runtime.ArtPackageStore");
        java.lang.reflect.Constructor<?> constructor = storeType.getDeclaredConstructor(Class.forName("com.trebledot.stone.runtime.ArtPackage")); constructor.setAccessible(true);
        Object store = constructor.newInstance(new Object[]{null});
        java.lang.reflect.Method prepare = storeType.getDeclaredMethod("prepare",android.content.pm.PackageManager.class,String.class,String.class); prepare.setAccessible(true);
        Object candidate = prepare.invoke(store,activity.getPackageManager(),"/data/route-target.apk",TARGET);
        try {
            java.lang.reflect.Method commit = storeType.getDeclaredMethod("commit",candidate.getClass(),String.class); commit.setAccessible(true);
            commit.invoke(store,candidate,SOURCE);
        } finally { ((AutoCloseable)candidate).close(); }
    }
    public static final class Source extends Activity {
        private int step;
        private boolean privateProcess;
        private int firstGamePid;
        private final Handler handler = new Handler();
        @Override protected void onCreate(Bundle saved) {
            super.onCreate(saved);
            android.widget.TextView text = new android.widget.TextView(this); text.setText("Stone Activity route source"); setContentView(text);
            ensure(getComponentName().equals(ALIAS),"Source alias lost");
            if (new File("/data/stone-route-private.txt").isFile()) {
                privateProcess=true; event(this,"private-source-pid-"+android.os.Process.myPid());
                notificationHost(this);
                handler.postDelayed(this::privateNext,250); return;
            }
            if (saved != null) { step = saved.getInt("step"); event(this,"source-recreated-"+step); return; }
            try { installTarget(this); } catch (Exception error) { throw new AssertionError("Signed route target setup failed",error); }
            event(this,"source-pid-"+android.os.Process.myPid());
            if (new File("/data/stone-route-resolution.txt").isFile()) resolution();
            if (new File("/data/stone-route-restart.txt").isFile()) step = 6;
            if (new File("/data/stone-route-back.txt").isFile()) step = 7;
            handler.postDelayed(this::next,250);
        }
        @Override protected void onSaveInstanceState(Bundle saved) { saved.putInt("step",step); super.onSaveInstanceState(saved); }
        private void resolution() {
            android.content.pm.PackageManager manager = getPackageManager();
            Intent connect = new Intent("info.guardianproject.panic.action.CONNECT").setPackage(TARGET);
            ensure(manager.queryIntentActivities(connect,0).size() == 1,"Non-launcher CONNECT filter was not discovered");
            java.util.List<android.content.pm.ResolveInfo> disabled = manager.queryIntentActivities(connect,android.content.pm.PackageManager.MATCH_DISABLED_COMPONENTS);
            ensure(disabled.size() == 2 && disabled.stream().anyMatch(info -> !info.activityInfo.enabled),"Disabled alias query policy changed");
            android.content.pm.ResolveInfo resolved = manager.resolveActivity(connect,android.content.pm.PackageManager.MATCH_DEFAULT_ONLY | android.content.pm.PackageManager.GET_RESOLVED_FILTER);
            ensure(resolved != null && resolved.activityInfo.name.equals(RECEIVER.getClassName()) && resolved.isDefault && resolved.filter != null,"CONNECT default metadata missing");
            ensure(resolved.priority == 0 && resolved.filter.getPriority() == 0,"Non-privileged positive filter priority was accepted");
            resolved.filter.addAction("com.trebledot.stone.route.MUTATED");
            ensure(manager.queryIntentActivities(new Intent("com.trebledot.stone.route.MUTATED").setPackage(TARGET),0).isEmpty(),"Returned filter mutated cached manifest");
            Intent noDefault = new Intent("com.trebledot.stone.route.NODEFAULT").setPackage(TARGET);
            ensure(manager.queryIntentActivities(noDefault,0).size() == 1
                && manager.queryIntentActivities(noDefault,android.content.pm.PackageManager.MATCH_DEFAULT_ONLY).isEmpty(),"MATCH_DEFAULT_ONLY ignored");
            Intent uri = new Intent("com.trebledot.stone.route.DATA").setPackage(TARGET)
                .setDataAndType(android.net.Uri.parse("stone-fixture://allowed.test/allowed/item"),"text/plain");
            ensure(manager.queryIntentActivities(uri,0).size() == 1,"Combined URI/MIME filter did not match");
            for (Intent bad : new Intent[]{new Intent(uri).setDataAndType(android.net.Uri.parse("stone-fixture://other.test/allowed/item"),"text/plain"),
                    new Intent(uri).setDataAndType(android.net.Uri.parse("stone-fixture://allowed.test/other/item"),"text/plain"),
                    new Intent(uri).setDataAndType(uri.getData(),"image/png"),new Intent(uri).addCategory("missing.category"),new Intent(uri).setPackage("com.trebledot.stone.route.absent")})
                ensure(manager.queryIntentActivities(bad,0).isEmpty(),"URI/MIME/category/package mismatch was accepted");
            Intent mime = new Intent("com.trebledot.stone.route.MIME").setPackage(TARGET)
                .setDataAndType(android.net.Uri.parse("content://fixture/item"),"image/png");
            ensure(manager.queryIntentActivities(mime,0).size() == 1,"MIME wildcard/content default scheme not matched");
            ensure(manager.queryIntentActivities(new Intent(mime).setDataAndType(android.net.Uri.parse("https://fixture/item"),"image/png"),0).isEmpty(),"MIME-only filter accepted an unrelated scheme");
            ensure(manager.queryIntentActivities(new Intent(connect).setComponent(RECEIVER).setPackage(SOURCE),0).isEmpty(),"Contradictory explicit package accepted");
            event(this,"public-filter-resolution-passed");
        }
        private void next() {
            String requestId = java.util.UUID.randomUUID().toString();
            Intent intent = new Intent("com.trebledot.stone.route."+MODES[step]).setComponent(RECEIVER)
                .putExtra("text","原始请求\nresult").putExtra("number",0x1020304050607080L).putExtra("enabled",true)
                .putExtra("bytes",new byte[]{0,1,-1}).putExtra(Intent.EXTRA_PACKAGE_NAME,"com.trebledot.stone.spoofed")
                .putExtra("fixtureRequestId",requestId);
            if (new File("/data/stone-route-resolution.txt").isFile()) intent.setComponent(null).setPackage(TARGET);
            event(this,"start-"+MODES[step]);
            if (step == 2) {
                startActivity(intent);
                handler.postDelayed(() -> { step++; next(); },1500);
            } else {
                startActivityForResult(intent,42+step);
                if (step == 0) handler.postDelayed(this::recreate,100);
                if (step == 5) {
                    long deadline = android.os.SystemClock.uptimeMillis()+60000;
                    handler.postDelayed(new Runnable() {
                        @Override public void run() {
                            File ready = new File("/data/stone-route-target-ready.txt");
                            try {
                                if (ready.isFile() && requestId.equals(new String(java.nio.file.Files.readAllBytes(ready.toPath()),StandardCharsets.UTF_8))) {
                                    event(Source.this,"source-finished-with-pending-result"); finish(); return;
                                }
                            } catch (Exception error) { throw new AssertionError(error); }
                            if (android.os.SystemClock.uptimeMillis() >= deadline) throw new AssertionError("Target did not initialize before caller finish");
                            handler.postDelayed(this,100);
                        }
                    },100);
                }
            }
        }
        @Override protected void onActivityResult(int request, int result, Intent data) {
            super.onActivityResult(request,result,data);
            if (privateProcess) {
                ensure(request==100+step,"Private process result request changed");
                if (step==1) ensure(result==RESULT_CANCELED && data==null,"Exited game did not cancel result");
                else {
                    ensure(result==RESULT_OK && data!=null,"Private game did not return");
                    Game value=(Game)data.getSerializableExtra("game"); int pid=data.getIntExtra("pid",0);
                    ensure(value!=null && value.id==7 && "Chessmaster".equals(value.title) && pid!=android.os.Process.myPid(),"Private game result or PID changed");
                    if(step==0) firstGamePid=pid; else ensure(firstGamePid!=pid,"Exited private process was reused");
                }
                event(this,"private-result-"+step+"-"+result);
                if(++step==3) { event(this,"private-complete"); finish(); }
                else handler.postDelayed(this::privateNext,250);
                return;
            }
            ensure(step != 5,"Target canceled before caller-revocation synchronization");
            ensure(request == 42+step,"Wrong or duplicate Activity result");
            if (step == 0 || step == 4 || step == 7) {
                ensure(result == RESULT_OK && data != null && "原始返回\nresult".equals(data.getStringExtra("text"))
                    && data.getLongExtra("number",0) == 0x1020304050607080L && data.getBooleanExtra("enabled",false)
                    && java.util.Arrays.equals(data.getByteArrayExtra("bytes"),new byte[]{0,1,-1}),"Activity result payload changed");
            } else ensure(result == RESULT_CANCELED && data == null,"Target cancellation or exit did not cancel");
            event(this,"result-"+MODES[step]+"-"+result);
            if (step >= 6) { finish(); return; }
            if (step == 4) {
                try {
                    startActivityForResult(new Intent().setComponent(new ComponentName(TARGET,PrivateTarget.class.getName())),99);
                    throw new AssertionError("Private Activity accepted");
                } catch (SecurityException expected) { event(this,"private-target-rejected"); }
                try {
                    startActivityForResult(new Intent().setComponent(RECEIVER).putExtra("parcelable",new ComponentName(SOURCE,"Fake")),99);
                    throw new AssertionError("Parcelable Activity payload accepted");
                } catch (UnsupportedOperationException expected) { event(this,"parcelable-rejected"); }
            }
            step++; handler.postDelayed(this::next,100);
        }
        private void privateNext() {
            startActivityForResult(new Intent().setComponent(new ComponentName(SOURCE,PrivateGame.class.getName()))
                .putExtra("game",new Game("Chessmaster",7)).putExtra("die",step==1).putExtra("sourcePid",android.os.Process.myPid()),100+step);
        }
    }
    public static final class Game implements java.io.Serializable {
        private static final long serialVersionUID=1L;
        final String title; final int id;
        Game(String title,int id) { this.title=title; this.id=id; }
    }
    public static final class PrivateGame extends Activity {
        @Override protected void onCreate(Bundle saved) {
            super.onCreate(saved);
            ensure((SOURCE+":game").equals(android.app.Application.getProcessName()),"Private Activity process name changed");
            ensure(getApplicationInfo().processName.equals(SOURCE),"Manifest default process was overwritten");
            ensure(ALIAS.equals(getCallingActivity()) && SOURCE.equals(getCallingPackage()),"Private Activity caller identity changed");
            ensure(android.os.Process.myPid()!=getIntent().getIntExtra("sourcePid",0),"Game ran in source process");
            Game value=(Game)getIntent().getSerializableExtra("game");
            ensure(value.id==7 && value.title.equals("Chessmaster"),"Application Serializable was not restored");
            event(this,"private-game-pid-"+android.os.Process.myPid());
            notificationHost(this);
            startService(new Intent(this,PrivateService.class));
            new Handler().postDelayed(() -> {
                ensure(PrivateService.started,"Private game service did not start in its Activity process");
                stopService(new Intent(this,PrivateService.class));
                if(getIntent().getBooleanExtra("die",false)) { event(this,"private-game-exit"); System.exit(0); return; }
                setResult(RESULT_OK,new Intent().putExtra("game",value).putExtra("pid",android.os.Process.myPid())); finish();
            },500);
        }
    }
    public static final class PrivateService extends android.app.Service {
        static boolean started;
        @Override public void onCreate() { super.onCreate(); started=true;
            ensure((SOURCE+":game").equals(android.app.Application.getProcessName()),"Service ran outside private process"); }
        @Override public android.os.IBinder onBind(Intent intent) { return null; }
        @Override public int onStartCommand(Intent intent,int flags,int id) { return START_NOT_STICKY; }
        @Override public void onDestroy() { started=false; super.onDestroy(); }
    }
    public static final class Target extends Activity {
        @Override protected void onCreate(Bundle saved) {
            super.onCreate(saved);
            String mode = getIntent().getAction().substring("com.trebledot.stone.route.".length());
            event(this,"target-pid-"+android.os.Process.myPid()+"-"+mode);
            if (mode.equals("ordinary")) {
                ensure(getCallingPackage() == null && getCallingActivity() == null,"Ordinary start acquired result caller");
                event(this,"ordinary-null-caller"); finish(); return;
            }
            ensure(SOURCE.equals(getCallingPackage()) && ALIAS.equals(getCallingActivity()),"Untrusted result caller identity");
            ensure("com.trebledot.stone.spoofed".equals(getIntent().getStringExtra(Intent.EXTRA_PACKAGE_NAME)),"Spoof control extra lost");
            ensure("原始请求\nresult".equals(getIntent().getStringExtra("text"))
                && getIntent().getLongExtra("number",0) == 0x1020304050607080L
                && getIntent().getBooleanExtra("enabled",false)
                && java.util.Arrays.equals(getIntent().getByteArrayExtra("bytes"),new byte[]{0,1,-1}),"Activity request payload changed");
            if (mode.equals("die")) { event(this,"target-exiting"); System.exit(0); return; }
            if (mode.equals("cancel")) { finish(); return; }
            if (mode.equals("back")) {
                new Handler().postDelayed(() -> {
                    setResult(RESULT_OK,new Intent().putExtra("text","原始返回\nresult").putExtra("number",0x1020304050607080L)
                        .putExtra("enabled",true).putExtra("bytes",new byte[]{0,1,-1}));
                    onBackPressed();
                },400); return;
            }
            if (mode.equals("service-restart")) {
                event(this,"restart-target-ready");
                Handler handler = new Handler();
                handler.postDelayed(new Runnable() {
                    @Override public void run() {
                        if (getCallingActivity() != null || getCallingPackage() != null) { handler.postDelayed(this,250); return; }
                        event(Target.this,"restart-caller-revoked"); finish();
                    }
                },250); return;
            }
            if (mode.equals("caller-gone")) {
                // Only test synchronization: identity was already checked via
                // the public Activity APIs above; the file never supplies it.
                try { java.nio.file.Files.write(new File("/data/stone-route-target-ready.txt").toPath(),getIntent().getStringExtra("fixtureRequestId").getBytes(StandardCharsets.UTF_8)); }
                catch (Exception error) { throw new AssertionError(error); }
                Handler handler = new Handler();
                handler.postDelayed(new Runnable() {
                    @Override public void run() {
                        if (getCallingActivity() != null || getCallingPackage() != null) { handler.postDelayed(this,250); return; }
                        event(Target.this,"caller-revoked"); finish();
                    }
                },250); return;
            }
            if (saved == null) { new Handler().postDelayed(this::recreate,400); return; }
            event(this,"target-recreated-"+mode);
            byte[] resultBytes = new byte[]{0,1,-1};
            setResult(RESULT_OK,new Intent().putExtra("text","原始返回\nresult").putExtra("number",0x1020304050607080L)
                .putExtra("enabled",true).putExtra("bytes",resultBytes)); finish();
            resultBytes[0] = 42; // Android must snapshot before finish() returns.
        }
        @Override public void finishAfterTransition() {
            if ("com.trebledot.stone.route.back".equals(getIntent().getAction())) event(this,"back-framework-finish");
            super.finishAfterTransition();
        }
        @Override protected void onDestroy() {
            if ("com.trebledot.stone.route.back".equals(getIntent().getAction())) {
                ensure(isFinishing(),"Back destroyed the Activity without framework finishing state"); event(this,"back-destroyed");
            }
            super.onDestroy();
        }
    }
    public static final class PrivateTarget extends Activity { }
    public static final class NoDefault extends Activity { }
}
