package com.trebledot.stone.runtime;

import android.app.PendingIntent;
import android.content.ComponentName;
import android.media.AudioAttributes;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** APK-local media session state. Audio output and Windows media integration are separate. */
final class ArtMediaSessions {
    private final ArtPackage apk;
    private final int user;
    private final List<Session> sessions = new ArrayList<>();
    ArtMediaSessions(ArtPackage apk, int user) { this.apk=apk; this.user=user; }
    static void initialize() throws ReflectiveOperationException {
        Class<?> initializer=Class.forName("android.media.MediaFrameworkPlatformInitializer");
        Class<?> manager=Class.forName("android.media.MediaServiceManager");
        if (initializer.getMethod("getMediaServiceManager").invoke(null)==null)
            initializer.getMethod("setMediaServiceManager",manager).invoke(null,manager.getConstructor().newInstance());
    }
    private void packageName(Object value) {
        if (!apk.applicationInfo.packageName.equals(value)) throw new SecurityException("Media sessions require the local APK identity");
    }
    synchronized Object call(Object instance, Method method, Object[] args) throws Exception {
        switch(method.getName()) {
            case "createSession": {
                packageName(args[0]);
                int callbackIndex=-1;
                for(int index=0;index<method.getParameterTypes().length;index++)
                    if(method.getParameterTypes()[index].getName().equals("android.media.session.ISessionCallback")) callbackIndex=index;
                if (callbackIndex<0 || (Integer)args[args.length-1]!=user || args[callbackIndex]==null)
                    throw new SecurityException("Invalid local media session owner");
                Session session=new Session((IInterface)args[callbackIndex],(String)args[callbackIndex+1],(Bundle)args[callbackIndex+2]);
                sessions.add(session); return session.owner;
            }
            case "isTrusted": return apk.applicationInfo.packageName.equals(args[0]) && (Integer)args[2]==apk.applicationInfo.uid;
            case "getSessions": {
                if ((Integer)args[1]!=user) return new ArrayList<MediaSession.Token>();
                List<MediaSession.Token> active=new ArrayList<>();
                for(Session session:sessions) if(session.active) active.add(session.token());
                return active;
            }
            case "getMediaKeyEventSession":
                packageName(args[0]); for(Session session:sessions) if(session.active) return session.token(); return null;
            case "getMediaKeyEventSessionPackageName":
                packageName(args[0]); for(Session session:sessions) if(session.active) return apk.applicationInfo.packageName; return "";
            case "isGlobalPriorityActive": return false;
            case "getSessionPolicies": return 0;
            default: throw new UnsupportedOperationException("ART media session manager: "+method.getName());
        }
    }
    private static Bundle copy(Bundle value) { return value==null ? null : new Bundle(value); }
    private final class Session {
        final IInterface owner,controller,callback;
        final String tag;
        final Bundle info;
        final Map<IBinder,IInterface> observers=new LinkedHashMap<>();
        volatile boolean active;
        boolean destroyed;
        long flags;
        MediaMetadata metadata;
        PlaybackState playback;
        PendingIntent launch,mediaButton;
        ComponentName receiver;
        Bundle extras;
        int ratingType;
        CharSequence queueTitle;
        List<?> queue;
        AudioAttributes attributes=new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
        Session(IInterface callback,String tag,Bundle info) throws ReflectiveOperationException {
            this.callback=callback; this.tag=tag; this.info=copy(info);
            controller=ArtServices.service("android.media.session.ISessionController",this::control).queryLocalInterface("android.media.session.ISessionController");
            owner=ArtServices.service("android.media.session.ISession",this::update).queryLocalInterface("android.media.session.ISession");
        }
        MediaSession.Token token() throws ReflectiveOperationException {
            return (MediaSession.Token)MediaSession.Token.class.getConstructor(int.class,Class.forName("android.media.session.ISessionController"))
                .newInstance(apk.applicationInfo.uid,controller);
        }
        void notifyControllers(String name,Object... args) throws ReflectiveOperationException {
            ArrayList<IInterface> snapshot;
            synchronized(this) { snapshot=new ArrayList<>(observers.values()); }
            for(Method method:Class.forName("android.media.session.ISessionControllerCallback").getMethods())
                if(method.getName().equals(name)) for(IInterface observer:snapshot) method.invoke(observer,args);
        }
        synchronized Object update(Object instance,Method method,Object[] args) throws Exception {
            if(destroyed) {
                if(method.getName().equals("destroy") || method.getName().equals("destroySession")) return null;
                throw new IllegalStateException("Media session has been released");
            }
            switch(method.getName()) {
                case "getController": return controller;
                case "setFlags": flags=((Number)args[0]).longValue(); return null;
                case "setActive": active=(Boolean)args[0]; return null;
                case "setMediaButtonReceiver": mediaButton=(PendingIntent)args[0]; return null;
                case "setMediaButtonBroadcastReceiver":
                    if(args[0]!=null) packageName(((ComponentName)args[0]).getPackageName());
                    receiver=(ComponentName)args[0]; return null;
                case "setLaunchPendingIntent": launch=(PendingIntent)args[0]; return null;
                case "setMetadata": metadata=(MediaMetadata)args[0]; notifyControllers("onMetadataChanged",metadata); return null;
                case "setPlaybackState": playback=(PlaybackState)args[0]; notifyControllers("onPlaybackStateChanged",playback); return null;
                case "setQueueTitle": queueTitle=(CharSequence)args[0]; notifyControllers("onQueueTitleChanged",queueTitle); return null;
                case "setExtras": extras=copy((Bundle)args[0]); notifyControllers("onExtrasChanged",copy(extras)); return null;
                case "setRatingType": ratingType=(Integer)args[0]; return null;
                case "resetQueue": queue=null; notifyControllers("onQueueChanged",(Object)null); return null;
                case "getBinderForSetQueue": {
                    java.util.function.Consumer<List<?>> consume=list -> {
                        synchronized(Session.this) { queue=new ArrayList<>(list); }
                        try { notifyControllers("onQueueChanged",Class.forName("android.content.pm.ParceledListSlice").getConstructor(List.class).newInstance(queue)); }
                        catch(ReflectiveOperationException error) { throw new IllegalStateException("Cannot publish media queue",error); }
                    };
                    Class<?> binder=Class.forName("android.media.session.ParcelableListBinder");
                    try { return binder.getConstructor(Class.class,java.util.function.Consumer.class).newInstance(MediaSession.QueueItem.class,consume); }
                    catch(NoSuchMethodException olderImage) { return binder.getConstructor(java.util.function.Consumer.class).newInstance(consume); }
                }
                case "setPlaybackToLocal": attributes=(AudioAttributes)args[0]; return null;
                case "sendEvent": notifyControllers("onEvent",args[0],copy((Bundle)args[1])); return null;
                case "destroy": case "destroySession":
                    active=false; destroyed=true; synchronized(ArtMediaSessions.this) { sessions.remove(this); }
                    notifyControllers("onSessionDestroyed"); observers.clear(); return null;
                default: throw new UnsupportedOperationException("ART media session: "+method.getName());
            }
        }
        synchronized Object control(Object instance,Method method,Object[] args) throws Exception {
            switch(method.getName()) {
                case "registerCallback": case "registerCallbackListener": {
                    IInterface observer=(IInterface)args[args.length-1];
                    if(destroyed) Class.forName("android.media.session.ISessionControllerCallback").getMethod("onSessionDestroyed").invoke(observer);
                    else observers.put(observer.asBinder(),observer);
                    return null;
                }
                case "unregisterCallback": case "unregisterCallbackListener": observers.remove(((IInterface)args[args.length-1]).asBinder()); return null;
                case "getPackageName": return apk.applicationInfo.packageName;
                case "getTag": return tag;
                case "getSessionInfo": return copy(info);
                case "getFlags": return flags;
                case "getLaunchPendingIntent": return launch;
                case "getMetadata": return metadata;
                case "getPlaybackState": return playback;
                case "getRatingType": return ratingType;
                case "getExtras": return copy(extras);
                case "getQueueTitle": return queueTitle;
                case "getQueue": return queue==null ? null : method.getReturnType().getConstructor(List.class).newInstance(queue);
                case "getVolumeAttributes":
                    return method.getReturnType().getConstructor(int.class,int.class,int.class,int.class,AudioAttributes.class,String.class)
                        .newInstance(1,0,ArtAudio.available()?15:0,ArtAudio.available()?15:0,attributes,null);
                case "play": case "pause": case "stop": case "prepare": case "next": case "previous":
                case "fastForward": case "rewind": case "seekTo": case "setPlaybackSpeed":
                case "playFromMediaId": case "playFromSearch": case "playFromUri": case "prepareFromMediaId":
                case "prepareFromSearch": case "prepareFromUri": case "skipToQueueItem": case "sendCustomAction":
                case "sendCommand": case "rate": {
                    if(destroyed) return null;
                    packageName(args[0]);
                    int offset=args.length>1 && method.getParameterTypes()[1].getName().equals("android.media.session.ISessionControllerCallback") ? 2 : 1;
                    Object[] forwarded=new Object[3+args.length-offset];
                    forwarded[0]=apk.applicationInfo.packageName; forwarded[1]=android.os.Process.myPid(); forwarded[2]=apk.applicationInfo.uid;
                    System.arraycopy(args,offset,forwarded,3,args.length-offset);
                    String name=method.getName();
                    name=name.equals("skipToQueueItem") ? "skipToTrack" : name.equals("sendCustomAction") ? "customAction" : name.equals("sendCommand") ? "command" : name;
                    for(Method target:Class.forName("android.media.session.ISessionCallback").getMethods())
                        if(target.getName().equals("on"+Character.toUpperCase(name.charAt(0))+name.substring(1))) return target.invoke(callback,forwarded);
                    throw new UnsupportedOperationException("Image media callback is unavailable: "+name);
                }
                default: throw new UnsupportedOperationException("ART media controller: "+method.getName());
            }
        }
    }
}
