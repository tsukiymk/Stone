package com.trebledot.stone.runtime;

import android.media.AudioManager;
import android.os.IInterface;
import java.lang.reflect.Method;
import java.util.HashSet;
import java.util.Set;
import java.util.Map;
import java.util.HashMap;

/** Client-local player bookkeeping and focus for Windows PCM streams. */
final class ArtAudio {
    static native boolean available();
    static int legacyStream(android.media.AudioAttributes attributes) throws ReflectiveOperationException {
        java.util.Objects.requireNonNull(attributes, "AudioAttributes");
        Method allFlags=android.media.AudioAttributes.class.getDeclaredMethod("getAllFlags");
        allFlags.setAccessible(true);
        int flags=(Integer)allFlags.invoke(attributes);
        if((flags&1)!=0) return 7; // enforced system sounds
        if((flags&4)!=0) return AudioManager.STREAM_VOICE_CALL; // SCO
        if((flags&8)!=0) return 9; // text-to-speech beacon
        // Android legacy volume streams for usage values 0 through 17. These
        // are volume-query mappings, not Android audio policy/HAL routes.
        final int[] streams={3,3,0,8,4,5,2,5,5,5,5,10,3,1,3,3,3,0};
        int usage=attributes.getUsage();
        return usage>=0 && usage<streams.length ? streams[usage] : AudioManager.STREAM_MUSIC;
    }
    private static Object outputPort() throws ReflectiveOperationException {
        Class<?> handleType=Class.forName("android.media.AudioHandle");
        java.lang.reflect.Constructor<?> handleCtor=handleType.getDeclaredConstructor(int.class);
        handleCtor.setAccessible(true); Object handle=handleCtor.newInstance(1);
        Class<?> gainArray=java.lang.reflect.Array.newInstance(Class.forName("android.media.AudioGain"),0).getClass();
        java.lang.reflect.Constructor<?> port=Class.forName("android.media.AudioDevicePort").getDeclaredConstructor(
            handleType,String.class,int[].class,int[].class,int[].class,int[].class,gainArray,int.class,String.class,int[].class,int[].class);
        port.setAccessible(true);
        return port.newInstance(handle,"Windows default output",new int[]{44100,48000},new int[]{4,12},new int[0],
            new int[]{2,3,4},java.lang.reflect.Array.newInstance(Class.forName("android.media.AudioGain"),0),
            0x1000000,"stone:windows-default",new int[0],new int[0]); // DEVICE_OUT_BUS: logical Windows route
    }
    private static int ports(java.util.ArrayList<Object> ports,int[] generation) throws ReflectiveOperationException {
        if(ports==null || generation==null || generation.length<1) return -22;
        ports.clear(); if(available()) ports.add(outputPort()); generation[0]=1; return 0;
    }
    private int nextPlayer=1;
    private final Set<Integer> players=new HashSet<>();
    private IInterface focusDispatcher;
    private String focusClient;
    private final Set<android.os.IBinder> loudnessDispatchers=new HashSet<>();
    private final Map<Integer,Set<Object>> loudnessSessions=new HashMap<>();
    private final Set<android.os.IBinder> spatializerDispatchers=new HashSet<>();

    synchronized Object call(Object instance,Method method,Object[] args) throws Exception {
        switch(method.getName()) {
            case "trackPlayer": { int id=nextPlayer++; players.add(id); return id; }
            case "releasePlayer": players.remove((Integer)args[0]); return null;
            case "playerAttributes": case "playerEvent": case "playerSessionId":
                if (!players.contains((Integer)args[0])) throw new IllegalArgumentException("Unknown local audio player");
                return null;
            // Windows owns endpoint volume. Android tracks retain their
            // own per-player gain without changing other Windows applications.
            case "isVolumeFixed": return true;
            case "getStreamVolume": case "getStreamMaxVolume": case "getLastAudibleStreamVolume": return available() ? 15 : 0;
            case "getStreamMinVolume": return 0;
            case "isStreamMute": case "isMasterMute": case "isMicrophoneMuted": case "isBluetoothScoOn":
            case "isBluetoothA2dpOn": case "isSpeakerphoneOn": case "isAudioServerRunning":
                return method.getName().equals("isAudioServerRunning") && available();
            case "getMode": return AudioManager.MODE_NORMAL;
            case "getRingerModeExternal": case "getRingerModeInternal": return AudioManager.RINGER_MODE_NORMAL;
            case "getDevicesForAttributes": return new java.util.ArrayList<>();
            case "registerPlaybackCallback": case "unregisterPlaybackCallback":
            case "registerRecordingCallback": case "unregisterRecordingCallback":
            case "registerAudioServerStateDispatcher": case "unregisterAudioServerStateDispatcher":
            case "registerModeDispatcher": case "unregisterModeDispatcher": return null;
            case "getActivePlaybackConfigurations": case "getActiveRecordingConfigurations": return new java.util.ArrayList<>();
            case "registerLoudnessCodecUpdatesDispatcher":
                loudnessDispatchers.add(((IInterface)args[0]).asBinder()); return null;
            case "unregisterLoudnessCodecUpdatesDispatcher":
                loudnessDispatchers.remove(((IInterface)args[0]).asBinder()); return null;
            case "startLoudnessCodecUpdates": {
                int session=(Integer)args[0];
                if(session<0) throw new IllegalArgumentException("Invalid audio session");
                loudnessSessions.computeIfAbsent(session,unused->new HashSet<>()); return null;
            }
            case "stopLoudnessCodecUpdates": loudnessSessions.remove((Integer)args[0]); return null;
            case "addLoudnessCodecInfo": {
                Set<Object> codecs=loudnessSessions.get((Integer)args[0]);
                if(codecs!=null) codecs.add(args[args.length-1]); return null;
            }
            case "removeLoudnessCodecInfo": {
                Set<Object> codecs=loudnessSessions.get((Integer)args[0]);
                if(codecs!=null) codecs.remove(args[args.length-1]); return null;
            }
            // Windows exposes no Android speaker-SPL calibration. Empty
            // overrides preserve the codec's own DRC defaults; do not invent
            // loudness parameters or device-change callbacks.
            case "getLoudnessParams": return new android.os.PersistableBundle();
            case "registerSpatializerCallback": spatializerDispatchers.add(((IInterface)args[0]).asBinder()); return null;
            case "unregisterSpatializerCallback": spatializerDispatchers.remove(((IInterface)args[0]).asBinder()); return null;
            case "getSpatializerImmersiveAudioLevel": return 0;
            case "isSpatializerEnabled": case "isSpatializerAvailable": case "isHeadTrackerAvailable":
            case "isSpatializerAvailableForDevice": case "canBeSpatialized": return false;
            case "getSpatializerCompatibleAudioDevices": return new java.util.ArrayList<>();
            case "getSupportedHeadTrackingModes": return new int[0];
            case "getActualHeadTrackingMode": case "getDesiredHeadTrackingMode": return -2;
            case "requestAudioFocus": {
                if (!available() || (Integer)args[1]!=AudioManager.AUDIOFOCUS_GAIN)
                    return AudioManager.AUDIOFOCUS_REQUEST_FAILED;
                IInterface dispatcher=null; int index=-1;
                for(int i=0;i<method.getParameterTypes().length;i++)
                    if(method.getParameterTypes()[i].getName().equals("android.media.IAudioFocusDispatcher")) {
                        dispatcher=(IInterface)args[i]; index=i;
                    }
                if(index<0 || args[index+1]==null) return AudioManager.AUDIOFOCUS_REQUEST_FAILED;
                String client=(String)args[index+1];
                if(focusDispatcher!=null && !client.equals(focusClient))
                    Class.forName("android.media.IAudioFocusDispatcher").getMethod("dispatchAudioFocusChange",int.class,String.class)
                        .invoke(focusDispatcher,AudioManager.AUDIOFOCUS_LOSS,focusClient);
                focusDispatcher=dispatcher; focusClient=client;
                return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
            }
            case "abandonAudioFocus":
                if(args.length>1 && java.util.Objects.equals(args[1],focusClient)) { focusClient=null; focusDispatcher=null; }
                return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
            default: throw new UnsupportedOperationException("ART audio service: "+method.getName());
        }
    }
}
