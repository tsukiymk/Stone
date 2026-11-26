package com.trebledot.stone.probe;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.WindowId;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import com.trebledot.stone.runtime.ArtWindow;
import java.lang.reflect.Method;
import java.util.Arrays;

/** Real image contracts used during Compose and Media3 application startup. */
public final class StartupServicesProbe {
    private static void check(boolean value,String text) { if(!value) throw new AssertionError(text); }
    public static int run(AssetManager assets,String stage) throws Exception {
        ArtPackage apk=ArtPackage.openApplication(assets,"/data/application.apk");
        if ("video".equals(stage)) {
            try { VideoCodecProbe.run(); } finally { apk.close(); }
            return 42;
        }
        Application.verifySharedMemory();
        BitmapFactoryProbe.verify(apk.resources.getAssets());
        GlSurfaceProbe.frameRateHints();
        check(!android.opengl.EGL14.EGL_NO_DISPLAY.equals(android.opengl.EGL14.eglGetDisplay(0)),
            "image EGL14 JNI default display");
        android.media.AudioAttributes mediaAttributes=new android.media.AudioAttributes.Builder()
            .setUsage(android.media.AudioAttributes.USAGE_MEDIA).build();
        Method legacyStream=Class.forName("android.media.audiopolicy.AudioProductStrategy")
            .getMethod("getLegacyStreamTypeForStrategyWithAudioAttributes",android.media.AudioAttributes.class);
        check((Integer)legacyStream.invoke(null,mediaAttributes)==AudioManager.STREAM_MUSIC,
            "image audio product strategy media stream mapping");
        int[] usages={android.media.AudioAttributes.USAGE_VOICE_COMMUNICATION,android.media.AudioAttributes.USAGE_ALARM,
            android.media.AudioAttributes.USAGE_NOTIFICATION,android.media.AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY};
        int[] streams={0,4,5,10};
        for(int i=0;i<usages.length;i++) check((Integer)legacyStream.invoke(null,
            new android.media.AudioAttributes.Builder().setUsage(usages[i]).build())==streams[i],"legacy usage stream mapping");
        check((Integer)legacyStream.invoke(null,new android.media.AudioAttributes.Builder().setFlags(1).build())==7,
            "enforced system sound stream mapping");
        java.util.ArrayList<Object> strategies=new java.util.ArrayList<>();
        check((Integer)Class.forName("android.media.audiopolicy.AudioProductStrategy")
            .getMethod("native_list_audio_product_strategies",java.util.ArrayList.class).invoke(null,strategies)==-38
            && strategies.isEmpty(),"absent Android audio policy discovery must report unsupported");
        Method inversePolicy=Class.forName("android.media.audiopolicy.AudioProductStrategy")
            .getDeclaredMethod("native_get_audio_attributes_for_legacy_stream",int.class,android.media.AudioAttributes.Builder.class);
        inversePolicy.setAccessible(true);
        check((Integer)inversePolicy.invoke(null,AudioManager.STREAM_MUSIC,new android.media.AudioAttributes.Builder())==-38,
            "absent inverse audio policy must report unsupported");
        android.media.AudioFormat compressedFormat=new android.media.AudioFormat.Builder()
            .setEncoding(android.media.AudioFormat.ENCODING_AAC_LC).setSampleRate(48000)
            .setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO).build();
        check(AudioManager.getPlaybackOffloadSupport(compressedFormat,mediaAttributes)==0,
            "compressed audio offload must remain unsupported");
        if ("native".equals(stage)) {
            apk.close();
            System.out.println("PASS image startup JNI: SharedMemory, BitmapRegionDecoder, EGL14 and audio strategy/offload queries.");
            return 42;
        }
        ArtApplication app=ArtApplication.start(apk);
        imageTimeZones();
        parcelTokens();
        AppOpsProbe.verify(app.context);
        StorageAccessProbe.verify(app);
        shortcuts(app);
        fileObservers(app);
        DocumentProbe.verify(app.context);
        ActivityIntentProbe.local();
        android.hardware.SensorManager sensors=app.context.getSystemService(android.hardware.SensorManager.class);
        check(sensors!=null && sensors.getSensorList(android.hardware.Sensor.TYPE_ALL).isEmpty(),"empty host sensor enumeration");
        check(sensors.getDefaultSensor(android.hardware.Sensor.TYPE_ACCELEROMETER)==null
            && sensors.getDefaultSensor(android.hardware.Sensor.TYPE_GYROSCOPE)==null,"no fabricated host motion sensor");
        check(sensors.getDynamicSensorList(android.hardware.Sensor.TYPE_ALL).isEmpty(),"empty dynamic host sensors");
        android.hardware.SensorEventListener sensorListener=new android.hardware.SensorEventListener() {
            public void onSensorChanged(android.hardware.SensorEvent event) { throw new AssertionError("No sensor events expected"); }
            public void onAccuracyChanged(android.hardware.Sensor sensor,int accuracy) { throw new AssertionError("No sensor events expected"); }
        };
        check(!sensors.registerListener(sensorListener,null,android.hardware.SensorManager.SENSOR_DELAY_GAME),"absent sensor listener must fail");
        sensors.unregisterListener(sensorListener);
        System.out.println("PASS image SensorManager empty enumeration/default/dynamic sensors and absent listener rejection");
        GlSurfaceProbe.run(app.context);
        GlSurfaceProbe.queuedUiOperations(app);
        EarlyFinishProbe.run(app);
        receivers(app);
        windows(app);
        media(app);
        AudioManager audio=app.context.getSystemService(AudioManager.class);
        check(audio.generateAudioSessionId()>0,"Windows audio session generation");
        check(audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS).length==1,"Windows logical default output device");
        int loudnessSession=audio.generateAudioSessionId();
        try(android.media.LoudnessCodecController first=android.media.LoudnessCodecController.create(loudnessSession);
                android.media.LoudnessCodecController second=android.media.LoudnessCodecController.create(audio.generateAudioSessionId())) {
            check(first!=null && second!=null,"concurrent image loudness controller registration");
            android.os.IBinder binder=(android.os.IBinder)Class.forName("android.os.ServiceManager")
                .getMethod("getService",String.class).invoke(null,"audio");
            Class<?> service=Class.forName("android.media.IAudioService"),info=Class.forName("android.media.LoudnessCodecInfo");
            Object codecInfo=info.getConstructor().newInstance(),owner=binder.queryLocalInterface(service.getName());
            service.getMethod("addLoudnessCodecInfo",int.class,int.class,info).invoke(owner,loudnessSession,71,codecInfo);
            Object parameters=service.getMethod("getLoudnessParams",info).invoke(owner,codecInfo);
            check(parameters instanceof android.os.PersistableBundle && ((android.os.PersistableBundle)parameters).isEmpty(),
                "unavailable Windows SPL calibration must preserve decoder defaults");
            service.getMethod("removeLoudnessCodecInfo",int.class,info).invoke(owner,loudnessSession,codecInfo);
        }
        try(android.media.LoudnessCodecController reopened=android.media.LoudnessCodecController.create(audio.generateAudioSessionId())) {
            check(reopened!=null,"loudness dispatcher re-registration after close");
        }
        System.out.println("PASS image loudness codec controllers create/close/re-register without device calibration overrides");
        android.media.Spatializer spatializer=audio.getSpatializer();
        android.media.Spatializer.OnSpatializerStateChangedListener listener=new android.media.Spatializer.OnSpatializerStateChangedListener() {
            public void onSpatializerEnabledChanged(android.media.Spatializer source,boolean enabled) { throw new AssertionError("No spatializer state change expected"); }
            public void onSpatializerAvailableChanged(android.media.Spatializer source,boolean available) { throw new AssertionError("No spatializer device change expected"); }
        };
        spatializer.addOnSpatializerStateChangedListener(Runnable::run,listener);
        check(!spatializer.isAvailable() && !spatializer.isEnabled()
            && spatializer.getImmersiveAudioLevel()==android.media.Spatializer.SPATIALIZER_IMMERSIVE_LEVEL_NONE
            && !spatializer.canBeSpatialized(new android.media.AudioAttributes.Builder().build(),new android.media.AudioFormat.Builder()
                .setEncoding(android.media.AudioFormat.ENCODING_PCM_16BIT).setSampleRate(48000).setChannelMask(android.media.AudioFormat.CHANNEL_OUT_STEREO).build()),
            "unsupported spatializer capabilities must not fail playback initialization");
        spatializer.removeOnSpatializerStateChangedListener(listener);
        System.out.println("PASS disabled spatializer capability queries and listener lifecycle");
        check(new android.media.MediaCodecList(android.media.MediaCodecList.ALL_CODECS).getCodecInfos().length>0,
            "available Windows audio decoders must be advertised");
        android.media.MediaCodec opus=android.media.MediaCodec.createDecoderByType("audio/opus");
        check(opus.getName().equals("c2.stone.opus.decoder") && !opus.getCodecInfo().isEncoder(),"real audio decoder identity"); opus.release();
        try { android.media.MediaCodec.createDecoderByType("video/av01"); throw new AssertionError("Unavailable decoder constructed"); }
        catch(java.io.IOException expected) {}
        System.out.println("PASS audio MediaCodec capabilities, creation/release and unsupported video rejection");
        System.out.println("PASS Windows audio sessions and logical output without Android Binder driver");
        return 42;
    }
    private static void imageTimeZones() {
        android.text.format.Time time=new android.text.format.Time("America/New_York");
        time.set(1704067200000L);
        check(time.year==2023 && time.month==11 && time.monthDay==31 && time.hour==19,"image winter timezone rules");
        time.set(1719792000000L);
        check(time.year==2024 && time.month==5 && time.monthDay==30 && time.hour==20,"image daylight timezone rules");
        java.text.SimpleDateFormat format=new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm",java.util.Locale.ROOT);
        format.setTimeZone(java.util.TimeZone.getTimeZone("Asia/Shanghai"));
        check("2024-01-01 08:00".equals(format.format(new java.util.Date(1704067200000L))),"image file date formatting");
        System.out.println("PASS image timezone database, winter/daylight rules and file date formatting");
    }
    private static void parcelTokens() throws Exception {
        Class<?> binder=android.os.Binder.class;
        Method getPolicy=binder.getDeclaredMethod("getThreadStrictModePolicy");
        Method setPolicy=binder.getDeclaredMethod("setThreadStrictModePolicy",int.class);
        Method setSource=binder.getDeclaredMethod("setCallingWorkSourceUid",int.class);
        Method getSource=binder.getDeclaredMethod("getCallingWorkSourceUid");
        Method restoreSource=binder.getDeclaredMethod("restoreCallingWorkSource",long.class);
        for(Method method:new Method[]{getPolicy,setPolicy,setSource,getSource,restoreSource}) method.setAccessible(true);
        int saved=(Integer)getPolicy.invoke(null);
        long token=(Long)setSource.invoke(null,31415);
        String descriptor="stone.local.\u4e2d\u6587\u0000\ud83d\udcc1";
        android.os.Parcel data=android.os.Parcel.obtain(),copy=android.os.Parcel.obtain();
        try {
            setPolicy.invoke(null,0);
            data.writeInterfaceToken(descriptor); data.writeInt(42);
            byte[] wire=data.marshall(); copy.unmarshall(wire,0,wire.length); copy.setDataPosition(0);
            check(copy.readInt()==Integer.MIN_VALUE && copy.readInt()==31415 && copy.readInt()==0x53595354
                && descriptor.equals(copy.readString()) && copy.readInt()==42,"image Parcel token wire format and UTF-16");
            data.setDataPosition(0); data.enforceInterface(descriptor);
            check(data.readInt()==42 && (Integer)getSource.invoke(null)==31415,"token cursor and calling work source");
            copy.setDataSize(0); copy.setDataPosition(0); copy.writeInterfaceToken(descriptor); copy.setDataPosition(0);
            copy.readInt(); check(copy.readInt()==-1,"received work source must not propagate implicitly");
            data.setDataPosition(0);
            try { data.enforceInterface(descriptor+"wrong"); throw new AssertionError("wrong interface accepted"); }
            catch(SecurityException expected) {}
            copy.setDataSize(0); copy.setDataPosition(0); copy.writeInt(0); copy.writeInt(-1); copy.writeInt(0); copy.writeString(descriptor);
            copy.setDataPosition(0);
            try { copy.enforceInterface(descriptor); throw new AssertionError("wrong header accepted"); }
            catch(SecurityException expected) {}
            copy.setDataSize(0); copy.setDataPosition(0); copy.writeInt(0); copy.setDataPosition(0);
            try { copy.enforceInterface(descriptor); throw new AssertionError("truncated token accepted"); }
            catch(SecurityException expected) {}
            restoreSource.invoke(null,token);
            long nested=(Long)setSource.invoke(null,2718); restoreSource.invoke(null,nested);
            check((Integer)getSource.invoke(null)==(int)token,"nested work source restore");
            System.out.println("PASS image Parcel interface tokens: UTF-16, wire header, cursor, work source and malformed rejection without Binder driver");
        } finally {
            data.recycle(); copy.recycle(); restoreSource.invoke(null,token);
            Class<?> strict=Class.forName("android.os.StrictMode");
            Method change=strict.getDeclaredMethod("onBinderStrictModePolicyChange",int.class); change.setAccessible(true); change.invoke(null,saved);
            setPolicy.invoke(null,saved);
        }
    }
    private static void shortcuts(ArtApplication app) throws Exception {
        android.content.pm.ShortcutManager manager=app.context.getSystemService(android.content.pm.ShortcutManager.class);
        check(manager!=null && !manager.isRequestPinShortcutSupported(),"image shortcut manager reports unavailable pinning");
        android.os.IBinder binder=(android.os.IBinder)Class.forName("android.os.ServiceManager")
            .getMethod("getService",String.class).invoke(null,"shortcut");
        Class<?> service=Class.forName("android.content.pm.IShortcutService");
        Object owner=binder.queryLocalInterface(service.getName());
        int user=(Integer)android.os.UserHandle.class.getMethod("myUserId").invoke(null);
        Method supported=service.getMethod("isRequestPinItemSupported",String.class,int.class,int.class);
        String pkg=app.context.getPackageName();
        check(Boolean.FALSE.equals(supported.invoke(owner,pkg,user,2)),"Android widget pinning is unavailable");
        for(Object[] caller:new Object[][]{{pkg,user+1,1},{"other.application",user,1}}) {
            try { supported.invoke(owner,caller); throw new AssertionError("foreign shortcut query accepted"); }
            catch(java.lang.reflect.InvocationTargetException denied) {
                if(!(denied.getCause() instanceof SecurityException)) throw denied;
            }
        }
        System.out.println("PASS image ShortcutManager availability, unsupported pinning and foreign caller denial");
    }
    private static void fileObservers(ArtApplication app) throws Exception {
        java.io.File first=new java.io.File(app.context.getFilesDir(),"observer-first");
        java.io.File second=new java.io.File(app.context.getFilesDir(),"observer-second");
        check(first.mkdirs() && second.mkdirs(),"observer fixture directories");
        java.util.concurrent.ConcurrentLinkedQueue<String> events=new java.util.concurrent.ConcurrentLinkedQueue<>();
        int mask=android.os.FileObserver.CREATE|android.os.FileObserver.DELETE|android.os.FileObserver.CLOSE_WRITE
            |android.os.FileObserver.MOVED_FROM|android.os.FileObserver.MOVED_TO;
        android.os.FileObserver observer=new android.os.FileObserver(Arrays.asList(first,second),mask) {
            public void onEvent(int event,String path) { events.add(event+":"+path); }
        };
        String original="偏好-📝.data",renamed="renamed.data";
        java.io.File file=new java.io.File(first,original),moved=new java.io.File(first,renamed);
        try {
            observer.startWatching();
            try(java.io.FileOutputStream output=new java.io.FileOutputStream(file)) {
                output.write(new byte[]{1,2,3}); output.flush();
                awaitFileEvent(events,android.os.FileObserver.CREATE,original);
                Thread.sleep(150);
                check(!events.contains(android.os.FileObserver.CLOSE_WRITE+":"+original),"open writer must not produce CLOSE_WRITE");
            }
            awaitFileEvent(events,android.os.FileObserver.CLOSE_WRITE,original);
            events.clear();
            try(java.io.FileInputStream input=new java.io.FileInputStream(file)) { check(input.read()==1,"observed content"); }
            Thread.sleep(150);
            check(events.isEmpty(),"read-only close and mask filtering");
            check(file.renameTo(moved),"observed rename");
            awaitFileEvent(events,android.os.FileObserver.MOVED_FROM,original);
            awaitFileEvent(events,android.os.FileObserver.MOVED_TO,renamed);
            check(moved.delete(),"observed delete");
            awaitFileEvent(events,android.os.FileObserver.DELETE,renamed);
            java.io.File other=new java.io.File(second,"second.data");
            try(java.io.FileOutputStream output=new java.io.FileOutputStream(other)) { output.write(4); }
            awaitFileEvent(events,android.os.FileObserver.CLOSE_WRITE,other.getName());
            observer.stopWatching(); events.clear();
            check(other.delete(),"stopped observer fixture delete"); Thread.sleep(150);
            check(events.isEmpty(),"stopWatching suppresses later callbacks");
            observer.startWatching();
            check(other.createNewFile(),"restart observer fixture");
            awaitFileEvent(events,android.os.FileObserver.CREATE,other.getName());
            observer.stopWatching(); check(other.delete(),"restart fixture cleanup");
        } finally {
            observer.stopWatching(); file.delete(); moved.delete(); first.delete(); second.delete();
        }
        System.out.println("PASS image FileObserver Unicode/multiple directories, create/rename/delete, real writable close, mask, stop and restart");
    }
    private static void awaitFileEvent(java.util.Collection<String> events,int mask,String name) throws Exception {
        long deadline=android.os.SystemClock.uptimeMillis()+5000;
        String expected=mask+":"+name;
        while(!events.contains(expected) && android.os.SystemClock.uptimeMillis()<deadline) Thread.sleep(10);
        check(events.contains(expected),"missing file event "+expected+" in "+events);
    }
    private static void receivers(ArtApplication app) throws Exception {
        PackageManager pm=app.context.getPackageManager(); String pkg=app.context.getPackageName();
        ComponentName receiver=new ComponentName(pkg,"com.trebledot.stone.cit.components.ComponentReceiver");
        ActivityInfo info=pm.getReceiverInfo(receiver,0);
        check(info.name.equals(receiver.getClassName()) && info.enabled && !info.exported,"receiver information");
        Intent intent=new Intent("com.trebledot.stone.cit.components.ORDERED").setPackage(pkg);
        java.util.List<android.content.pm.ResolveInfo> matches=pm.queryBroadcastReceivers(intent,PackageManager.GET_RESOLVED_FILTER);
        check(matches.size()==1 && matches.get(0).activityInfo.name.equals(info.name)
            && matches.get(0).filter.hasAction(intent.getAction()) && matches.get(0).priority==0,"receiver filter and bounded priority");
        check(pm.queryBroadcastReceivers(new Intent("unmatched.receiver.action").setPackage(pkg),0).isEmpty(),"unmatched receiver action");
        check(pm.queryBroadcastReceivers(new Intent(intent).addCategory("missing.receiver.category"),0).isEmpty(),"unmatched receiver category");
        ComponentName disabled=new ComponentName(pkg,receiver.getClassName()+"$Disabled");
        try { pm.getReceiverInfo(disabled,0); throw new AssertionError("disabled receiver visible without flag"); }
        catch(PackageManager.NameNotFoundException expected) {}
        check(!pm.getReceiverInfo(disabled,PackageManager.MATCH_DISABLED_COMPONENTS).enabled,"disabled receiver query flag");
        check(pm.queryBroadcastReceivers(new Intent().setComponent(disabled),PackageManager.MATCH_DISABLED_COMPONENTS).size()==1,"explicit disabled receiver query");
        try { pm.getReceiverInfo(new ComponentName(pkg,app.apk.activityInfo.name),0); throw new AssertionError("activity treated as receiver"); }
        catch(PackageManager.NameNotFoundException expected) {}
        Intent service=new Intent("com.trebledot.stone.cit.components.SERVICE_QUERY").setPackage(pkg);
        java.util.List<android.content.pm.ResolveInfo> services=pm.queryIntentServices(service,PackageManager.GET_RESOLVED_FILTER);
        check(services.size()==1 && services.get(0).serviceInfo.name.equals("com.trebledot.stone.cit.components.ComponentService")
            && services.get(0).filter.hasAction(service.getAction()),"manifest service filter query");
        check(pm.queryIntentServices(new Intent(service).addCategory("missing.service.category"),0).isEmpty(),"unmatched service category");
        ComponentName disabledService=new ComponentName(pkg,"com.trebledot.stone.cit.components.ComponentService$Disabled");
        check(pm.queryIntentServices(new Intent().setComponent(disabledService),0).isEmpty(),"disabled service query");
        check(pm.queryIntentServices(new Intent().setComponent(disabledService),PackageManager.MATCH_DISABLED_COMPONENTS).size()==1,"explicit disabled service flag");
        check(pm.getPackageInfo(pkg,PackageManager.GET_SERVICES).services.length>=4,"package service metadata");
        System.out.println("PASS receiver/service metadata, real manifest filters, explicit queries and disabled-component boundaries");
    }
    private static void windows(ArtApplication app) throws Exception {
        Activity activity=app.startActivity();
        android.app.ActivityManager.RunningAppProcessInfo memory=new android.app.ActivityManager.RunningAppProcessInfo();
        android.app.ActivityManager.getMyMemoryState(memory);
        check(memory.pid==android.os.Process.myPid() && memory.uid==app.apk.applicationInfo.uid
            && memory.importance==android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND,"current APK process identity and Activity importance");
        System.out.println("PASS current process memory state uses real local Activity importance");
        WindowId[] duringAttach={null};
        View view=new View(activity) { @Override protected void onAttachedToWindow() {
            super.onAttachedToWindow(); duringAttach[0]=getWindowId();
        }
            @Override public void onWindowFocusChanged(boolean focused) {
                super.onWindowFocusChanged(focused);
                check(getWindowId().isFocused()==focused,"WindowId state inside View focus callback");
            }
        };
        activity.setContentView(view);
        Class<?> services=Class.forName("com.trebledot.stone.runtime.ArtServices");
        java.lang.reflect.Field field=services.getDeclaredField("windows"); field.setAccessible(true); Object windows=field.get(null);
        Method focus=windows.getClass().getDeclaredMethod("focusChanged",View.class,boolean.class); focus.setAccessible(true);
        int[] events={0,0};
        WindowId.FocusObserver observer=new WindowId.FocusObserver() {
            public void onFocusGained(WindowId id) { events[0]++; }
            public void onFocusLost(WindowId id) { events[1]++; }
        };
        AutoCloseable root=ArtWindow.attach(activity);
        WindowId id=view.getWindowId();
        try {
            check(id!=null && id.equals(duringAttach[0]) && id.equals(view.getWindowId()),"stable WindowId during attach");
            check(!id.isFocused(),"window initially unfocused"); id.registerFocusObserver(observer);
            focus.invoke(windows,activity.getWindow().getDecorView(),true); BroadcastProbe.drain();
            check(id.isFocused() && events[0]==1 && events[1]==0,"focus gained callback and identity");
            focus.invoke(windows,activity.getWindow().getDecorView(),true); BroadcastProbe.drain();
            check(events[0]==1,"unchanged focus must not duplicate notification");
            focus.invoke(windows,activity.getWindow().getDecorView(),false); BroadcastProbe.drain();
            check(!id.isFocused() && events[1]==1,"focus lost callback");
            id.unregisterFocusObserver(observer);
            focus.invoke(windows,activity.getWindow().getDecorView(),true); BroadcastProbe.drain();
            check(events[0]==1,"unregistered focus observer");
        } finally { root.close(); app.destroyActivity(); }
        check(!id.isFocused(),"detached window loses focus");
        System.out.println("PASS WindowId during attach, stable identity, real focus callbacks, unregister and detach");
    }
    private static void media(ArtApplication app) throws Exception {
        MediaSession session=new MediaSession(app.context,"Stone startup probe");
        int[] events={0,0,0,0};
        session.setCallback(new MediaSession.Callback() { public void onPlay() { events[0]++; } public void onSeekTo(long position) { if(position==321) events[1]++; } },new Handler(Looper.getMainLooper()));
        MediaController controller=new MediaController(app.context,session.getSessionToken());
        MediaController.Callback observer=new MediaController.Callback() {
            public void onMetadataChanged(MediaMetadata value) { if(value!=null) events[2]++; }
            public void onSessionDestroyed() { events[3]++; }
        };
        controller.registerCallback(observer,new Handler(Looper.getMainLooper()));
        try {
            session.setActive(true);
            check(controller.getPackageName().equals(app.context.getPackageName()),"session controller owner");
            session.setMetadata(new MediaMetadata.Builder().putString(MediaMetadata.METADATA_KEY_TITLE,"Stone media probe").build());
            session.setPlaybackState(new PlaybackState.Builder().setState(PlaybackState.STATE_PAUSED,123,0).build());
            Bundle extras=new Bundle(); extras.putString("probe","before"); session.setExtras(extras); extras.putString("probe","after");
            session.setQueue(Arrays.asList(new MediaSession.QueueItem(new android.media.MediaDescription.Builder().setMediaId("probe-id").setTitle("Probe").build(),42)));
            session.setQueueTitle("Probe queue"); BroadcastProbe.drain();
            check("Stone media probe".equals(controller.getMetadata().getString(MediaMetadata.METADATA_KEY_TITLE)) && events[2]==1,"media metadata and controller callback");
            check(controller.getPlaybackState().getPosition()==123 && controller.getPlaybackState().getState()==PlaybackState.STATE_PAUSED,"playback state roundtrip");
            check("before".equals(controller.getExtras().getString("probe")),"media extras snapshot");
            check(controller.getQueue().size()==1 && controller.getQueue().get(0).getQueueId()==42 && "Probe queue".contentEquals(controller.getQueueTitle()),"media queue roundtrip");
            check(controller.getPlaybackInfo().getMaxVolume()==15,"Windows fixed system-volume range");
            controller.getTransportControls().play(); controller.getTransportControls().seekTo(321); BroadcastProbe.drain();
            check(events[0]==1 && events[1]==1,"local transport callback dispatch");
            MediaSessionManager manager=app.context.getSystemService(MediaSessionManager.class);
            check(manager.getActiveSessions(null).size()==1,"local active session query");
        } finally { session.release(); BroadcastProbe.drain(); controller.unregisterCallback(observer); }
        check(events[3]==1,"session release callback");
        check(app.context.getSystemService(MediaSessionManager.class).getActiveSessions(null).isEmpty(),"released session removed");
        System.out.println("PASS local media session creation, metadata/state/queue, controller callbacks, transport commands and release");
    }
}
