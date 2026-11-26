package com.trebledot.stone.runtime;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.os.Handler;
import android.os.PersistableBundle;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/** Windows MFT decoders; Android owns buffers, Surface lifetimes and callbacks. */
public final class ArtMediaCodecs {
    private ArtMediaCodecs() {}
    private static native int available();
    private static native long open(boolean opus,int rate,int channels,byte[] csd);
    private static native byte[] decode(long handle,byte[] packet,long[] pts,boolean eos);
    private static native void flushDecoder(long handle);
    private static native void closeDecoder(long handle);
    private static native boolean videoSurface(android.view.Surface surface);
    private static native long openVideo(boolean hevc,int width,int height,byte[] headers);
    private static native long[] decodeVideo(long handle,byte[] packet,long pts,boolean eos);
    private static native void renderVideo(int[] pixels,int width,int height,android.view.Surface surface,long timestampNs);
    private static native void renderVideoFrame(long frame,android.view.Surface surface,long timestampNs);
    private static native void releaseVideoFrame(long frame);
    private static native void flushVideo(long handle);
    private static native void closeVideo(long handle);
    // MediaCodec frees each returned native view when it invalidates a buffer.
    // Keep allocation ownership in State, independent of those disposable views.
    private static native ByteBuffer view(ByteBuffer allocation);
    private static final String[] NAMES={"c2.stone.opus.decoder","c2.stone.avc.decoder","c2.stone.hevc.decoder","c2.stone.aac.decoder"};
    private static final String[] TYPES={"audio/opus","video/avc","video/hevc","audio/mp4a-latm"};
    private static final AtomicLong next=new AtomicLong(1);
    private static final Map<Long,State> states=new ConcurrentHashMap<>();
    private static final Field context=field(MediaCodec.class,"mNativeContext");
    private static final Method post=method(MediaCodec.class,"postEventFromNative",int.class,int.class,int.class,Object.class);
    private static Field field(Class<?> type,String name) {
        try { Field f=type.getDeclaredField(name); f.setAccessible(true); return f; }
        catch(ReflectiveOperationException e) { throw new IllegalStateException(e); }
    }
    private static Method method(Class<?> type,String name,Class<?>... args) {
        try { Method m=type.getDeclaredMethod(name,args); m.setAccessible(true); return m; }
        catch(ReflectiveOperationException e) { throw new IllegalStateException(e); }
    }
    private static long id(MediaCodec codec) {
        try { return context.getLong(codec); } catch(IllegalAccessException e) { throw new IllegalStateException(e); }
    }
    private static void id(MediaCodec codec,long value) {
        try { context.setLong(codec,value); } catch(IllegalAccessException e) { throw new IllegalStateException(e); }
    }
    private static State state(MediaCodec codec) {
        State s=states.get(id(codec)); if(s==null) throw new IllegalStateException("Released codec"); return s;
    }
    private static int physicalIndex(int index) {
        int mask=available(); for(int i=0;i<NAMES.length;i++) if((mask&(1<<i))!=0 && index--==0) return i;
        throw new IllegalArgumentException("Invalid codec index");
    }
    public static int count() { return Integer.bitCount(available()); }
    public static String name(int index) { return NAMES[physicalIndex(index)]; }
    public static String[] types(int index) { return new String[]{TYPES[physicalIndex(index)]}; }
    public static int find(String name) { for(int i=0;i<count();i++) if(name(i).equals(name)) return i; return -1; }
    public static MediaCodecInfo.CodecCapabilities capabilities(int index,String mime) {
        int physical=physicalIndex(index);
        if(!TYPES[physical].equalsIgnoreCase(mime)) throw new IllegalArgumentException("Unsupported codec MIME type");
        try {
            Map<String,Object> defaults=new HashMap<>(); defaults.put("mime",mime);
            Map<String,Object> info=new HashMap<>();
            info.put("sample-rate-ranges","48000");
            info.put("channel-ranges","1-2"); info.put("max-channel-count","2");
            info.put("bitrate-range","6000-510000");
            info.put("max-concurrent-instances","16");
            MediaCodecInfo.CodecProfileLevel[] profiles=new MediaCodecInfo.CodecProfileLevel[0];
            int[] colors=new int[0];
            if(physical==3) {
                info.put("sample-rate-ranges","8000,11025,12000,16000,22050,24000,32000,44100,48000,64000,88200,96000");
                info.put("bitrate-range","8000-512000");
                profiles=new MediaCodecInfo.CodecProfileLevel[]{new MediaCodecInfo.CodecProfileLevel()};
                profiles[0].profile=MediaCodecInfo.CodecProfileLevel.AACObjectLC;
            }
            if(physical==1 || physical==2) {
                // Caps round maximum dimensions down to whole coding blocks.
                // 1080 visible lines require a 1088-line coded-height bound.
                info.clear(); info.put("size-range","2x2-1920x1088"); info.put("alignment","2x2");
                info.put("block-size","16x16"); info.put("block-count-range","1-8160");
                info.put("blocks-per-second-range","1-489600"); info.put("frame-rate-range","1-60");
                info.put("bitrate-range","1-50000000"); info.put("max-concurrent-instances","4");
                profiles=new MediaCodecInfo.CodecProfileLevel[3];
                for(int i=0;i<3;i++) { profiles[i]=new MediaCodecInfo.CodecProfileLevel(); profiles[i].profile=new int[]{1,2,8}[i]; profiles[i].level=0x2000; }
                if(physical==2) { profiles=new MediaCodecInfo.CodecProfileLevel[]{new MediaCodecInfo.CodecProfileLevel()}; profiles[0].profile=MediaCodecInfo.CodecProfileLevel.HEVCProfileMain; profiles[0].level=MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel41; }
                colors=new int[]{0x7f000789};
            }
            Class<?> implementation=Class.forName("android.media.MediaCodecInfo$CodecCapabilities$CodecCapsLegacyImpl");
            Constructor<?> ctor=implementation.getDeclaredConstructor(MediaCodecInfo.CodecProfileLevel[].class,int[].class,boolean.class,Map.class,Map.class);
            ctor.setAccessible(true); Object impl=ctor.newInstance(profiles,colors,false,defaults,info);
            Class<?> intf=Class.forName("android.media.MediaCodecInfo$CodecCapabilities$CodecCapsIntf");
            Constructor<MediaCodecInfo.CodecCapabilities> wrapper=MediaCodecInfo.CodecCapabilities.class.getDeclaredConstructor(intf);
            wrapper.setAccessible(true); return wrapper.newInstance(impl);
        } catch(ReflectiveOperationException e) { throw new IllegalStateException("Unsupported image codec capabilities ABI",e); }
    }
    private static final class Frame {
        final int index; final MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
        Frame(int i,int length,long pts,int flags) { index=i; info.set(0,length,pts,flags); }
    }
    private static final class State {
        final WeakReference<MediaCodec> codec; final int type;
        long decoder; boolean configured,started,async,eos,formatPending;
        int channels,rate,generation;
        int width,height;
        android.view.Surface surface;
        Handler renderHandler;
        RuntimeException renderFailure;
        long[] videoFrames;
        int[] videoWidths,videoHeights;
        long[] videoTimes;
        ByteBuffer[] inputs,outputs;
        boolean[] inputOwned,outputOwned;
        final ArrayDeque<Integer> freeInputs=new ArrayDeque<>(),freeOutputs=new ArrayDeque<>();
        final ArrayDeque<Frame> ready=new ArrayDeque<>();
        Map<String,Object> format;
        State(MediaCodec owner,int type) { codec=new WeakReference<>(owner); this.type=type; }
        boolean video() { return type==1 || type==2; }
        void event(int kind,int index,Object value) {
            MediaCodec owner=codec.get(); if(owner==null) return;
            try { post.invoke(owner,1,kind,index,value); }
            catch(ReflectiveOperationException e) { throw new IllegalStateException(e); }
        }
        void clearEvents() {
            if(renderHandler!=null) renderHandler.removeCallbacksAndMessages(this);
            MediaCodec owner=codec.get(); if(owner==null) return;
            try { Handler handler=(Handler)field(MediaCodec.class,"mCallbackHandler").get(owner); if(handler!=null) handler.removeMessages(1); }
            catch(IllegalAccessException e) { throw new IllegalStateException(e); }
        }
        void requireStarted() { if(renderFailure!=null) throw new IllegalStateException("Video frame output failed",renderFailure); if(!started || !configured) throw new IllegalStateException("Codec is not started"); }
        void clearBuffers() {
            generation++;
            freeInputs.clear(); freeOutputs.clear(); ready.clear();
            for(int i=0;i<inputs.length;i++) { inputs[i].clear(); inputOwned[i]=false; freeInputs.add(i); }
            for(int i=0;i<outputs.length;i++) { if(outputs[i]!=null) outputs[i].clear(); if(videoFrames!=null) { releaseVideoFrame(videoFrames[i]); videoFrames[i]=0; } outputOwned[i]=false; freeOutputs.add(i); }
            renderFailure=null;
            eos=false; formatPending=true;
        }
        void offerInputs() {
            int outstanding=0; for(boolean owned:inputOwned) if(owned) outstanding++;
            if(async) while(!freeInputs.isEmpty() && !eos && freeOutputs.size()>outstanding) { int i=freeInputs.removeFirst(); inputOwned[i]=true; outstanding++; event(1,i,null); }
        }
        boolean inputAvailable() {
            int outstanding=0; for(boolean owned:inputOwned) if(owned) outstanding++;
            return !freeInputs.isEmpty() && freeOutputs.size()>outstanding;
        }
        void closeNative() { if(decoder!=0) { if(video()) closeVideo(decoder); else closeDecoder(decoder); } decoder=0; }
        void dispose() { generation++; started=false; configured=false; clearEvents(); closeNative(); videoFrames=null; surface=null; notifyAll(); }
    }
    public static void setup(MediaCodec codec,String value,boolean byType,boolean encoder) throws IOException {
        if(encoder) throw new IOException("Stone supports decoding only");
        int physical=-1;
        for(int i=0;i<NAMES.length;i++) if((available()&(1<<i))!=0 && (byType ? TYPES[i] : NAMES[i]).equals(value)) physical=i;
        if(physical<0) throw new IOException("Unsupported Stone codec: "+value);
        long key=next.getAndIncrement(); states.put(key,new State(codec,physical)); id(codec,key);
    }
    private static int number(Map<String,Object> values,String key,int fallback) { Object v=values.get(key); return v instanceof Number ? ((Number)v).intValue() : fallback; }
    private static byte[] bytes(Object value) {
        if(value==null) return new byte[0]; ByteBuffer source=((ByteBuffer)value).duplicate(); byte[] data=new byte[source.remaining()]; source.get(data); return data;
    }
    public static void configure(MediaCodec codec,String[] keys,Object[] values,Object surface,Object crypto,Object descrambler,int flags) {
        State s=state(codec); synchronized(s) {
            // Media3 requests asynchronous crypto callbacks even for clear
            // streams. With no crypto object this flag needs no DRM transport.
            if(s.started || (!s.video() && surface!=null) || crypto!=null || descrambler!=null || (flags&~4)!=0) throw new IllegalArgumentException("Unsupported codec mode or encrypted content");
            Map<String,Object> map=new HashMap<>(); for(int i=0;i<keys.length;i++) map.put(keys[i],values[i]);
            if(!TYPES[s.type].equals(map.get("mime"))) throw new IllegalArgumentException("Codec MIME mismatch");
            int rate=number(map,"sample-rate",48000),channels=number(map,"channel-count",2);
            if(s.type==0 && rate!=48000) throw new IllegalArgumentException("Opus output must use 48000 Hz");
            byte[] csd=bytes(map.get("csd-0"));
            if(s.type==3) {
                int[] rates={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000};
                if(csd.length<2 || csd.length>64 || (csd[0]>>>3&31)!=2) throw new IllegalArgumentException("Only AAC-LC AudioSpecificConfig is supported");
                int frequency=(csd[0]&7)*2+(csd[1]>>>7&1),channelConfig=csd[1]>>>3&15;
                if(frequency>=rates.length || rates[frequency]!=rate || channelConfig!=channels || channels<1 || channels>2)
                    throw new IllegalArgumentException("AAC-LC rate/channel configuration mismatch");
            }
            if(s.type==0 && (csd.length<19 || !new String(csd,0,8,java.nio.charset.StandardCharsets.US_ASCII).equals("OpusHead") || csd[18]!=0))
                throw new IllegalArgumentException("Only mono/stereo Opus mapping family zero is supported");
            s.closeNative();
            s.decoder=0; s.configured=false;
            byte[] delay=bytes(map.get("csd-1"));
            if(s.type==0 && delay.length==8) {
                long nanos=ByteBuffer.wrap(delay).order(ByteOrder.LITTLE_ENDIAN).getLong();
                if(nanos<0 || nanos>65535L*1000000000L/rate) throw new IllegalArgumentException("Invalid Opus codec delay");
                int skip=(int)(nanos*rate/1000000000L); csd[10]=(byte)skip; csd[11]=(byte)(skip>>8);
            }
            // The Windows Opus MFT applies OpusHead pre-skip itself.
            if(s.video()) {
                if(!(surface instanceof android.view.Surface) || !videoSurface((android.view.Surface)surface))
                    throw new IllegalArgumentException("Video decoding requires a Stone software Surface");
                s.width=number(map,"width",0); s.height=number(map,"height",0);
                byte[] extension=bytes(map.get("csd-1")),headers=java.util.Arrays.copyOf(csd,csd.length+extension.length);
                System.arraycopy(extension,0,headers,csd.length,extension.length);
                s.decoder=openVideo(s.type==2,s.width,s.height,headers); s.surface=(android.view.Surface)surface;
                if(Boolean.getBoolean("stone.diagnostics")) android.util.Log.i("StoneMediaCodec","Configured "+NAMES[s.type]+" "+s.width+"x"+s.height);
                s.renderHandler=new Handler(android.os.Looper.getMainLooper());
            } else s.decoder=open(s.type==0,rate,channels,csd);
            s.rate=rate; s.channels=channels;
            int size=Math.max(65536,Math.min(1048576,number(map,"max-input-size",65536)));
            int slots=s.video() ? Math.max(4,Math.min(32,67108864/(s.width*s.height*4))) : 8;
            s.inputs=new ByteBuffer[4]; s.outputs=new ByteBuffer[slots];
            s.inputOwned=new boolean[4]; s.outputOwned=new boolean[s.outputs.length];
            for(int i=0;i<s.inputs.length;i++) s.inputs[i]=ByteBuffer.allocateDirect(size).order(ByteOrder.nativeOrder());
            for(int i=0;i<s.outputs.length;i++) if(!s.video()) s.outputs[i]=ByteBuffer.allocateDirect(262144).order(ByteOrder.nativeOrder());
            if(s.video()) { s.videoFrames=new long[s.outputs.length]; s.videoWidths=new int[s.outputs.length]; s.videoHeights=new int[s.outputs.length]; s.videoTimes=new long[s.outputs.length]; }
            s.format=new HashMap<>(); s.format.put("mime","audio/raw"); s.format.put("sample-rate",rate); s.format.put("channel-count",channels); s.format.put("pcm-encoding",2);
            if(s.video()) { s.format.clear(); s.format.put("mime","video/raw"); s.format.put("width",s.width); s.format.put("height",s.height); s.format.put("color-format",0x7f000789); }
            s.configured=true; s.clearBuffers();
        }
    }
    public static void callback(MediaCodec codec,Object callback) { State s=state(codec); synchronized(s) { if(s.started) throw new IllegalStateException("Codec already started"); s.async=callback!=null; } }
    public static void start(MediaCodec codec) { State s=state(codec); synchronized(s) { if(!s.configured || s.started) throw new IllegalStateException("Codec cannot start"); s.started=true; if(s.async) { s.event(4,0,new HashMap<>(s.format)); s.formatPending=false; s.offerInputs(); } } }
    public static void flush(MediaCodec codec) { State s=state(codec); synchronized(s) { s.requireStarted(); s.clearEvents(); if(s.video()) flushVideo(s.decoder); else flushDecoder(s.decoder); s.clearBuffers(); if(s.async) s.started=false; s.notifyAll(); } }
    public static void stop(MediaCodec codec) { State s=state(codec); synchronized(s) { s.dispose(); } }
    public static void release(MediaCodec codec) { State s=states.remove(id(codec)); id(codec,0); if(s!=null) synchronized(s) { s.dispose(); } }
    public static ByteBuffer[] buffers(MediaCodec codec,boolean input) { State s=state(codec); synchronized(s) { if(!s.configured) throw new IllegalStateException("Codec is not configured"); if(!input && s.video()) return null; ByteBuffer[] original=input ? s.inputs : s.outputs,result=new ByteBuffer[original.length]; for(int i=0;i<result.length;i++) result[i]=view(original[i]).order(ByteOrder.nativeOrder()); return result; } }
    public static ByteBuffer buffer(MediaCodec codec,boolean input,int index) { State s=state(codec); synchronized(s) { s.requireStarted(); boolean[] owned=input ? s.inputOwned : s.outputOwned; if(index<0 || index>=owned.length || !owned[index]) throw new IllegalStateException("Buffer is not owned by the caller"); if(!input && s.video()) return null; ByteBuffer allocation=(input ? s.inputs : s.outputs)[index],result=view(allocation).order(ByteOrder.nativeOrder()); result.limit(input ? allocation.capacity() : allocation.limit()); return result; } }
    public static Map<String,Object> format(MediaCodec codec,boolean input) { State s=state(codec); synchronized(s) { if(!s.configured) throw new IllegalStateException("Codec is not configured"); Map<String,Object> map=new HashMap<>(s.format); if(input) map.put("mime",TYPES[s.type]); return map; } }
    private static boolean waitFor(State s,long deadline) {
        long remaining=deadline-System.nanoTime(); if(remaining<=0) return false;
        try { s.wait(remaining/1000000,(int)(remaining%1000000)); return true; } catch(InterruptedException e) { Thread.currentThread().interrupt(); throw new IllegalStateException("Interrupted codec",e); }
    }
    private static long deadline(long timeout) { return timeout<0 ? Long.MAX_VALUE : System.nanoTime()+Math.min(timeout,60000000L)*1000; }
    public static int input(MediaCodec codec,long timeout) { State s=state(codec); synchronized(s) { s.requireStarted(); if(s.async) throw new IllegalStateException("Asynchronous codec"); long end=deadline(timeout); while(!s.inputAvailable() && !s.eos) { if(!waitFor(s,end)) return -1; s.requireStarted(); } if(s.eos) return -1; int i=s.freeInputs.removeFirst(); s.inputOwned[i]=true; return i; } }
    public static int output(MediaCodec codec,MediaCodec.BufferInfo info,long timeout) { State s=state(codec); synchronized(s) { s.requireStarted(); if(s.async) throw new IllegalStateException("Asynchronous codec"); if(s.formatPending) { s.formatPending=false; return -2; } long end=deadline(timeout); while(s.ready.isEmpty()) { if(!waitFor(s,end)) return -1; s.requireStarted(); } Frame frame=s.ready.removeFirst(); s.outputOwned[frame.index]=true; info.set(frame.info.offset,frame.info.size,frame.info.presentationTimeUs,frame.info.flags); return frame.index; } }
    public static void queue(MediaCodec codec,int index,int offset,int size,long pts,int flags) { State s=state(codec); synchronized(s) {
        s.requireStarted(); if(s.eos || index<0 || index>=s.inputs.length || !s.inputOwned[index]) throw new IllegalStateException("Invalid input buffer ownership");
        if(offset<0 || size<0 || offset>s.inputs[index].capacity()-size || (flags&~7)!=0) throw new IllegalArgumentException("Invalid codec input buffer");
        byte[] data=new byte[size]; ByteBuffer source=s.inputs[index].duplicate(); source.clear().position(offset).limit(offset+size); source.get(data);
        boolean eos=(flags&4)!=0; long[] time={pts}; int generation=s.generation;
        if(s.video()) {
            long[] frames=decodeVideo(s.decoder,(flags&2)!=0 ? new byte[0] : data,pts,eos);
            int count=frames.length/4;
            for(int at=0;at<count || (eos && at==0);at++) {
                long end=System.nanoTime()+5000000000L;
                while(s.freeOutputs.isEmpty()) { if(!waitFor(s,end)) throw new IllegalStateException("Video output buffers were not released"); s.requireStarted(); if(generation!=s.generation) throw new IllegalStateException("Video input interrupted by flush"); }
                int out=s.freeOutputs.removeFirst();
                long frameId=count==0 ? 0 : frames[at*4],frameTime=count==0 ? pts : frames[at*4+1];
                int width=count==0 ? s.width : (int)frames[at*4+2],height=count==0 ? s.height : (int)frames[at*4+3];
                s.videoFrames[out]=frameId; s.videoWidths[out]=width; s.videoHeights[out]=height;
                s.videoTimes[out]=frameTime*1000;
                Frame frame=new Frame(out,frameId==0 ? 0 : width*height*4,frameTime,eos && at==count-1 || eos && count==0 ? 4 : 0);
                if(s.async) { s.outputOwned[out]=true; s.event(2,out,frame.info); } else s.ready.add(frame);
                s.notifyAll();
            }
            s.inputOwned[index]=false; s.inputs[index].clear(); s.eos=eos; s.freeInputs.add(index); s.offerInputs(); s.notifyAll();
            return;
        }
        byte[] pcm=(flags&2)!=0 && !eos ? new byte[0] : decode(s.decoder,(flags&2)!=0 ? new byte[0] : data,time,eos);
        int frameBytes=s.channels*2,begin=0;
        while(begin<pcm.length || eos) {
            long end=System.nanoTime()+5000000000L;
            while(s.freeOutputs.isEmpty()) { if(!waitFor(s,end)) throw new IllegalStateException("Audio output buffers were not released"); s.requireStarted(); if(generation!=s.generation) throw new IllegalStateException("Audio input interrupted by flush"); }
            int out=s.freeOutputs.removeFirst(); ByteBuffer target=s.outputs[out]; target.clear();
            int length=Math.min(target.capacity(),pcm.length-begin); target.put(pcm,begin,length); target.flip();
            boolean last=begin+length==pcm.length; Frame frame=new Frame(out,length,time[0],eos&&last ? 4 : 0);
            if(s.async) { s.outputOwned[out]=true; s.event(2,out,frame.info); } else s.ready.add(frame);
            begin+=length; time[0]+=length/frameBytes*1000000L/s.rate; s.notifyAll(); if(last) break;
        }
        s.inputOwned[index]=false; s.inputs[index].clear(); s.eos=eos; s.freeInputs.add(index); s.offerInputs(); s.notifyAll();
    } }
    public static void releaseOutput(MediaCodec codec,int index,boolean render,long timestampNs) { State s=state(codec); synchronized(s) {
        s.requireStarted(); if((render && !s.video()) || index<0 || index>=s.outputs.length || !s.outputOwned[index]) throw new IllegalStateException("Invalid output buffer release");
        if(render && s.videoFrames[index]!=0) {
            final int generation=s.generation; final android.view.Surface target=s.surface;
            long delay=timestampNs<0 ? 0 : Math.max(0,(timestampNs-System.nanoTime())/1000000);
            if(delay>60000) throw new IllegalArgumentException("Video render timestamp exceeds scheduling limit");
            boolean posted=s.renderHandler.postDelayed(()->{ synchronized(s) {
                if(generation!=s.generation || !s.started) return;
                try { if(s.surface==target && target.isValid()) renderVideoFrame(s.videoFrames[index],target,timestampNs<0 ? s.videoTimes[index] : timestampNs); }
                catch(RuntimeException failure) { s.renderFailure=failure; }
                finally { releaseVideoFrame(s.videoFrames[index]); s.videoFrames[index]=0; s.freeOutputs.add(index); if(s.renderFailure==null) s.offerInputs(); s.notifyAll(); }
            } },s,delay);
            if(!posted) throw new IllegalStateException("Video output Looper has stopped");
            s.outputOwned[index]=false;
        } else { s.outputOwned[index]=false; if(s.videoFrames!=null) { releaseVideoFrame(s.videoFrames[index]); s.videoFrames[index]=0; } s.freeOutputs.add(index); s.offerInputs(); s.notifyAll(); }
    } }
    public static void surface(MediaCodec codec,android.view.Surface target) { State s=state(codec); synchronized(s) { if(!s.configured || !s.video() || !videoSurface(target)) throw new IllegalArgumentException("Invalid software output Surface"); s.surface=target; } }
    public static void scaling(MediaCodec codec,int mode) { state(codec); if(mode!=1) throw new UnsupportedOperationException("Only scale-to-fit video output is supported"); }
    public static String canonical(MediaCodec codec) { return NAMES[state(codec).type]; }
    public static MediaCodecInfo info(MediaCodec codec) { String name=canonical(codec); for(MediaCodecInfo value:new android.media.MediaCodecList(1).getCodecInfos()) if(value.getName().equals(name)) return value; throw new IllegalStateException("Codec disappeared"); }
    public static PersistableBundle metrics(MediaCodec codec) { state(codec); return new PersistableBundle(); }
    public static void parameters(MediaCodec codec,String[] keys,Object[] values) {
        state(codec);
        for(int i=0;i<keys.length;i++) {
            // Scheduling hints do not change this software decoder's format.
            if((keys[i].equals("importance") || keys[i].equals("priority") || keys[i].equals("operating-rate")) && values[i] instanceof Number) continue;
            throw new UnsupportedOperationException("Unsupported codec parameter: "+keys[i]);
        }
    }
}
