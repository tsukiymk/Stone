package com.trebledot.stone.probe;

import android.content.res.AssetManager;
import android.media.*;
import com.trebledot.stone.runtime.ArtApplication;
import com.trebledot.stone.runtime.ArtPackage;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicInteger;

/** Exercises the selected image AudioTrack, never a replacement Java track. */
public final class AudioOutputProbe {
    private static void check(boolean value,String text) { if(!value) throw new AssertionError(text); }
    private static AudioTrack track(int encoding,int channels,int bytes) {
        return track(encoding,channels,bytes,48000);
    }
    private static AudioTrack track(int encoding,int channels,int bytes,int rate) {
        return new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build())
            .setAudioFormat(new AudioFormat.Builder().setEncoding(encoding).setSampleRate(rate).setChannelMask(channels).build())
            .setBufferSizeInBytes(bytes).setTransferMode(AudioTrack.MODE_STREAM).build();
    }
    public static int run(AssetManager assets,String unused) throws Exception {
        try { int result=execute(assets); if("codecs".equals(unused)) AudioCodecProbe.run(); return result; }
        catch(Throwable error) {
            error.printStackTrace(System.err);
            try(java.io.PrintWriter file=new java.io.PrintWriter("/data/audio-error.txt")) { error.printStackTrace(file); }
            return -1;
        }
    }
    private static int execute(AssetManager assets) throws Exception {
        ArtApplication app=ArtApplication.start(ArtPackage.openApplication(assets,"/data/application.apk"));
        AudioManager manager=app.context.getSystemService(AudioManager.class);
        check(manager.generateAudioSessionId()>0,"Windows audio session");
        check(manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).length==1,"logical Windows output");
        check(manager.isVolumeFixed() && manager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)==15,"Windows endpoint volume ownership");
        AudioFocusRequest focus=new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN).build();
        check(manager.requestAudioFocus(focus)==AudioManager.AUDIOFOCUS_REQUEST_GRANTED,"local audio focus");
        manager.abandonAudioFocusRequest(focus);
        parcels();
        check(AudioTrack.getMinBufferSize(48000,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT)>0,"minimum PCM buffer");
        AudioTrack audio=track(AudioFormat.ENCODING_PCM_16BIT,AudioFormat.CHANNEL_OUT_STEREO,19200);
        check(audio.getState()==AudioTrack.STATE_INITIALIZED && audio.getBufferCapacityInFrames()==4800,"real image AudioTrack setup");
        java.lang.reflect.Method latency=AudioTrack.class.getDeclaredMethod("getLatency"); latency.setAccessible(true);
        int latencyMillis=(Integer)latency.invoke(audio);
        check(latencyMillis>=0,"WASAPI stream latency");
        check(audio.setBufferSizeInFrames(2400)==2400 && (Integer)latency.invoke(audio)==latencyMillis,
            "output latency must not be the application's queue capacity");
        check(audio.setBufferSizeInFrames(4800)==4800,"restore logical buffer size");
        System.out.println("WASAPI reported output latency="+latencyMillis+" ms (zero if unavailable)");
        short[] pcm=new short[48000*2];
        for(int i=0;i<48000;i++) {
            pcm[i*2]=(short)(3000*Math.sin(i*2*Math.PI*997/48000));
            pcm[i*2+1]=(short)(3000*Math.sin(i*2*Math.PI*1499/48000));
        }
        audio.setVolume(0.5f);
        audio.play();
        check(audio.write(pcm,0,pcm.length,AudioTrack.WRITE_BLOCKING)==pcm.length,"blocking PCM16 write");
        Thread.sleep(180);
        check(audio.getPlaybackHeadPosition()>=47000,"hardware playback position");
        AudioTimestamp stamp=new AudioTimestamp();
        check(audio.getTimestamp(stamp) && stamp.framePosition>=47000 && stamp.nanoTime>0,"monotonic hardware timestamp");
        audio.pause(); int position=audio.getPlaybackHeadPosition(); Thread.sleep(80);
        check(audio.getPlaybackHeadPosition()==position,"pause preserves position");
        audio.flush(); check(audio.getPlaybackHeadPosition()==0,"flush resets position");
        int queued=audio.write(pcm,0,pcm.length,AudioTrack.WRITE_NON_BLOCKING);
        check(queued==9600 && audio.write(pcm,0,2,AudioTrack.WRITE_NON_BLOCKING)==0,"bounded nonblocking writes");
        audio.stop(); audio.release();
        check(audio.getState()==AudioTrack.STATE_UNINITIALIZED,"release");
        AudioTrack floats=track(AudioFormat.ENCODING_PCM_FLOAT,AudioFormat.CHANNEL_OUT_MONO,19200);
        floats.play(); float[] samples=new float[9600];
        for(int i=0;i<samples.length;i++) samples[i]=(float)(0.06*Math.sin(i*2*Math.PI*661/48000));
        check(floats.write(samples,0,samples.length,AudioTrack.WRITE_BLOCKING)==samples.length,"float write");
        Thread.sleep(130); floats.stop(); floats.release();
        AudioTrack direct=track(AudioFormat.ENCODING_PCM_8BIT,AudioFormat.CHANNEL_OUT_MONO,4800);
        ByteBuffer bytes=ByteBuffer.allocateDirect(9600).order(ByteOrder.nativeOrder());
        for(int i=0;i<bytes.capacity();i++) bytes.put((byte)(128+12*Math.sin(i*2*Math.PI*443/48000)));
        bytes.flip(); direct.play();
        check(direct.write(bytes,bytes.remaining(),AudioTrack.WRITE_BLOCKING)==9600 && !bytes.hasRemaining(),"direct buffer position and PCM8");
        Thread.sleep(130); direct.stop(); direct.release();
        AudioTrack resampled=track(AudioFormat.ENCODING_PCM_16BIT,AudioFormat.CHANNEL_OUT_STEREO,17640,44100);
        ByteBuffer pcmDirect=ByteBuffer.allocateDirect(17640).order(ByteOrder.nativeOrder());
        for(int i=0;i<4410;i++) { short value=(short)(1000*Math.sin(i*2*Math.PI*337/44100)); pcmDirect.putShort(value).putShort(value); }
        pcmDirect.flip(); resampled.play();
        check(resampled.write(pcmDirect,pcmDirect.remaining(),AudioTrack.WRITE_BLOCKING)==17640,"44.1 kHz direct PCM16");
        Thread.sleep(160); resampled.stop(); resampled.release();
        liveVolume();
        for(String action:new String[]{"pause","stop","release"}) interruptedWriter(action);
        System.out.println("PASS image AudioTrack PCM16 stereo, float/PCM8 mono, direct buffers, gain, hardware position/timestamp, pause/flush/stop/release and bounded writes");
        return 42;
    }
    private static void interruptedWriter(String action) throws Exception {
        AudioTrack interrupted=track(AudioFormat.ENCODING_PCM_16BIT,AudioFormat.CHANNEL_OUT_STEREO,19200);
        interrupted.play(); AtomicInteger result=new AtomicInteger(Integer.MIN_VALUE);
        Thread writer=new Thread(()->result.set(interrupted.write(new short[48000*8],0,48000*8,AudioTrack.WRITE_BLOCKING)));
        writer.start(); long deadline=android.os.SystemClock.uptimeMillis()+5000;
        while(interrupted.getPlaybackHeadPosition()==0 && writer.isAlive() && android.os.SystemClock.uptimeMillis()<deadline) Thread.sleep(10);
        check(interrupted.getPlaybackHeadPosition()>0,"writer reached the audio device");
        if(action.equals("pause")) interrupted.pause();
        else if(action.equals("stop")) interrupted.stop();
        else interrupted.release();
        writer.join(2000);
        check(!writer.isAlive() && result.get()>0 && result.get()<48000*8,action+" interrupts blocked writer");
        interrupted.release();
    }
    private static boolean hasBinders(android.os.Parcel parcel) throws Exception {
        java.lang.reflect.Method method=android.os.Parcel.class.getDeclaredMethod("hasBinders");
        method.setAccessible(true); return (Boolean)method.invoke(parcel);
    }
    private static void waitPosition(AudioTrack track,int frames) throws Exception {
        long deadline=android.os.SystemClock.uptimeMillis()+5000;
        while(track.getPlaybackHeadPosition()<frames && android.os.SystemClock.uptimeMillis()<deadline) Thread.sleep(10);
        check(track.getPlaybackHeadPosition()>=frames,"live volume playback progress");
    }
    private static void liveVolume() throws Exception {
        AudioTrack first=track(AudioFormat.ENCODING_PCM_16BIT,AudioFormat.CHANNEL_OUT_MONO,96000);
        AudioTrack second=track(AudioFormat.ENCODING_PCM_16BIT,AudioFormat.CHANNEL_OUT_MONO,96000);
        try {
            short[] a=new short[48000],b=new short[48000];
            for(int i=0;i<a.length;i++) {
                a[i]=(short)(1200*Math.sin(i*2*Math.PI*2711/48000));
                b[i]=(short)(800*Math.sin(i*2*Math.PI*3121/48000));
            }
            check(first.write(a,0,a.length)==a.length && second.write(b,0,b.length)==b.length,"prequeued independent streams");
            first.play(); second.play(); waitPosition(first,12000);
            first.setVolume(0); waitPosition(first,30000); first.setVolume(1);
            waitPosition(first,48000); Thread.sleep(100);
        } finally { first.release(); second.release(); }
    }
    private static void parcels() throws Exception {
        android.os.IBinder binder=new android.os.Binder();
        android.os.Parcel source=android.os.Parcel.obtain(),copy=android.os.Parcel.obtain();
        try {
            source.writeInt(71); source.writeStrongBinder(binder); source.writeString("after");
            check(hasBinders(source),"local Binder indexed by Parcel");
            source.setDataPosition(0);
            check(source.readInt()==71 && source.readStrongBinder()==binder && "after".equals(source.readString()),"local Binder identity and cursor");
            copy.appendFrom(source,0,source.dataSize()); copy.setDataPosition(0);
            check(copy.readInt()==71 && copy.readStrongBinder()==binder,"append retains local Binder ownership");
            try { source.marshall(); throw new AssertionError("Binder token escaped into bytes"); }
            catch(android.os.BadParcelableException expected) {}
            source.setDataSize(4); check(!hasBinders(source),"truncation releases local Binder references");
        } finally { source.recycle(); copy.recycle(); }
        android.os.Parcel reused=android.os.Parcel.obtain();
        try { check(!hasBinders(reused),"recycle releases local Binder references"); reused.writeStrongBinder(null); reused.setDataPosition(0); check(reused.readStrongBinder()==null,"nullable local Binder"); }
        finally { reused.recycle(); }
        System.out.println("PASS local Binder Parcel round trip, append, recycle and marshalling boundary");
    }
}
