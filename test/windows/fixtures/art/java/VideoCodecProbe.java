package com.trebledot.stone.probe;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.view.Surface;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;

/** Encodes no application data: GenerateAvc supplies red and blue H.264 frames. */
final class VideoCodecProbe {
    private static void check(boolean value,String message) { if(!value) throw new AssertionError(message); }
    private static final class Packet {
        final long pts; final byte[] bytes;
        Packet(ByteBuffer file) { pts=file.getLong(); int size=file.getInt(); check(size>0 && size<=file.remaining(),"AVC packet length"); bytes=new byte[size]; file.get(bytes); }
    }
    static void run() throws Exception {
        for(android.media.MediaCodecInfo info:new android.media.MediaCodecList(1).getCodecInfos()) {
            for(String mime:info.getSupportedTypes()) if(mime.startsWith("video/")) {
                android.media.MediaCodecInfo.CodecCapabilities caps=info.getCapabilitiesForType(mime);
                MediaFormat fullHd=MediaFormat.createVideoFormat(mime,1920,1080); fullHd.setFloat("frame-rate",24f);
                fullHd.setInteger("bitrate",2109718);
                System.out.println("Video caps "+mime+" widths="+caps.getVideoCapabilities().getSupportedWidths()+" heights="+caps.getVideoCapabilities().getSupportedHeights()+" rates="+caps.getVideoCapabilities().getSupportedFrameRates());
                check(caps.isFormatSupported(fullHd),"Advertised 1080p24 capability rejected: "+mime);
            }
        }
        ByteBuffer file=ByteBuffer.wrap(Files.readAllBytes(Paths.get("/data/avc-fixture.bin"))).order(ByteOrder.LITTLE_ENDIAN);
        byte[] magic=new byte[8]; file.get(magic);
        check("STAVC001".equals(new String(magic,java.nio.charset.StandardCharsets.US_ASCII)),"AVC fixture version");
        int width=file.getInt(),height=file.getInt(),headerSize=file.getInt(),count=file.getInt();
        check(width==1920 && height==1080 && count==24 && headerSize>=0 && headerSize<=65536,"AVC fixture dimensions/count");
        byte[] headers=new byte[headerSize]; file.get(headers);
        ArrayList<Packet> packets=new ArrayList<>(); for(int i=0;i<count;i++) packets.add(new Packet(file));
        check(!file.hasRemaining(),"Trailing AVC fixture data");
        Class<?> views=Class.forName("com.trebledot.stone.runtime.ArtSurfaceViews");
        Method create=views.getDeclaredMethod("createNative",Surface.class,int.class,int.class),pixels=views.getDeclaredMethod("pixelsNative",long.class);
        create.setAccessible(true); pixels.setAccessible(true);
        java.lang.reflect.Constructor<Surface> constructor=Surface.class.getDeclaredConstructor(); constructor.setAccessible(true);
        Surface surface=constructor.newInstance(); long token=(Long)create.invoke(null,surface,width,height);
        MediaCodec codec=MediaCodec.createDecoderByType("video/avc");
        try {
            check(codec.getName().equals("c2.stone.avc.decoder"),"Windows AVC decoder identity");
            MediaFormat format=MediaFormat.createVideoFormat("video/avc",width,height);
            format.setByteBuffer("csd-0",ByteBuffer.wrap(headers));
            codec.configure(format,surface,null,0); codec.start();
            for(int pass=0;pass<2;pass++) {
                int sent=0,received=0; boolean eos=false; long end=System.nanoTime()+10000000000L;
                while(!eos && System.nanoTime()<end) {
                    int input=codec.dequeueInputBuffer(0);
                    if(input>=0) {
                        if(sent<packets.size()) {
                            Packet packet=packets.get(sent++); codec.getInputBuffer(input).put(packet.bytes);
                            codec.queueInputBuffer(input,0,packet.bytes.length,packet.pts,0);
                        } else { check(received>0,"Decoder buffered the complete stream until EOS"); codec.queueInputBuffer(input,0,0,1000000,MediaCodec.BUFFER_FLAG_END_OF_STREAM); }
                    }
                    MediaCodec.BufferInfo info=new MediaCodec.BufferInfo(); int output;
                    while((output=codec.dequeueOutputBuffer(info,0))!=MediaCodec.INFO_TRY_AGAIN_LATER) {
                        if(output==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) { check(codec.getOutputFormat().getInteger("width")==width,"AVC output format"); continue; }
                        check(output>=0,"Unexpected AVC output event");
                        if(info.size>0) {
                            check(codec.getOutputBuffer(output)==null,"Surface mode exposed a byte buffer");
                            check(received<count && Math.abs(info.presentationTimeUs-packets.get(received).pts)<=1,"AVC frame timestamp "+received+": "+info.presentationTimeUs);
                            if(pass==0 && received==12) {
                                codec.releaseOutputBuffer(output,System.nanoTime()+80000000L);
                                BroadcastProbe.drain();
                                check(pixels.invoke(null,token)==null,"Timestamped frame appeared before its deadline");
                                Thread.sleep(100); BroadcastProbe.drain();
                            } else { codec.releaseOutputBuffer(output,true); BroadcastProbe.drain(); }
                            color((int[])pixels.invoke(null,token),width,height,received<12);
                            received++;
                        } else codec.releaseOutputBuffer(output,false);
                        eos=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                    }
                }
                check(eos && received==count,"AVC EOS/frame count "+received);
                if(pass==0) codec.flush();
            }
            codec.stop(); check(surface.isValid(),"Codec stop released caller Surface");
            codec.configure(format,surface,null,0); codec.start();
            Packet first=packets.get(0); int input=codec.dequeueInputBuffer(1000000); check(input>=0,"Reconfigure input");
            codec.getInputBuffer(input).put(first.bytes); codec.queueInputBuffer(input,0,first.bytes.length,first.pts,0);
            input=codec.dequeueInputBuffer(1000000); codec.queueInputBuffer(input,0,0,1000000,MediaCodec.BUFFER_FLAG_END_OF_STREAM);
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo(); int output;
            do { output=codec.dequeueOutputBuffer(info,1000000); } while(output==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED);
            check(output>=0 && info.size>0,"Flush cancellation output frame");
            codec.releaseOutputBuffer(output,System.nanoTime()+80000000L); codec.flush();
            Thread.sleep(100); BroadcastProbe.drain(); check(pixels.invoke(null,token)==null,"Flushed scheduled frame was displayed");
            codec.stop(); codec.configure(format,surface,null,0);
            int[] sent={0},received={0}; boolean[] inputEos={false},outputEos={false}; Throwable[] failure={null};
            codec.setCallback(new MediaCodec.Callback() {
                @Override public void onInputBufferAvailable(MediaCodec owner,int index) {
                    try {
                        if(inputEos[0]) return;
                        if(sent[0]<count) {
                            Packet packet=packets.get(sent[0]++); owner.getInputBuffer(index).put(packet.bytes);
                            owner.queueInputBuffer(index,0,packet.bytes.length,packet.pts,0);
                        } else { inputEos[0]=true; owner.queueInputBuffer(index,0,0,1000000,MediaCodec.BUFFER_FLAG_END_OF_STREAM); }
                    } catch(Throwable error) { failure[0]=error; }
                }
                @Override public void onOutputBufferAvailable(MediaCodec owner,int index,MediaCodec.BufferInfo info) {
                    try {
                        if(info.size>0) {
                            check(received[0]<count && Math.abs(info.presentationTimeUs-packets.get(received[0]).pts)<=1,"Async AVC PTS");
                            received[0]++;
                        }
                        owner.releaseOutputBuffer(index,info.size>0);
                        if((info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0) outputEos[0]=true;
                    } catch(Throwable error) { failure[0]=error; }
                }
                @Override public void onOutputFormatChanged(MediaCodec owner,MediaFormat output) { check(output.getInteger("height")==height,"Async AVC geometry"); }
                @Override public void onError(MediaCodec owner,MediaCodec.CodecException error) { failure[0]=error; }
            });
            codec.start(); long end=System.nanoTime()+10000000000L;
            while(!outputEos[0] && failure[0]==null && System.nanoTime()<end) { BroadcastProbe.drain(); Thread.sleep(1); }
            if(failure[0]!=null) throw new AssertionError("Async AVC callbacks",failure[0]);
            check(outputEos[0] && received[0]==count,"Async AVC EOS/frame count "+received[0]);
            BroadcastProbe.drain(); color((int[])pixels.invoke(null,token),width,height,false);
            System.out.println("PASS Windows AVC decoder: sync/async actual red/blue frames, PTS, EOS, scheduled Surface pixels, flush replay/cancellation, stop/reconfigure and ownership");
        } finally { codec.release(); surface.release(); }
        SurfaceTextureProbe.run();
        AacCodecProbe.run();
    }
    private static void color(int[] pixels,int width,int height,boolean red) {
        check(pixels!=null && pixels.length==width*height,"Missing actual AVC Surface pixels");
        int value=pixels[width*(height/2)+width/2],r=value>>>16&255,g=value>>>8&255,b=value&255;
        check((value>>>24)==255 && g<35 && (red ? r>220 && b<35 : b>220 && r<35),"Decoded AVC color: "+Integer.toHexString(value)+", red="+red);
    }
}
