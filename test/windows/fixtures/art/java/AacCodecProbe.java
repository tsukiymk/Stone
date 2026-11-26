package com.trebledot.stone.probe;

import android.media.MediaCodec;
import android.media.MediaFormat;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Paths;

/** Independent Windows encoder fixture checks real AAC-LC PCM and decoder lifecycle. */
final class AacCodecProbe {
    private static void check(boolean value,String message) { if(!value) throw new AssertionError(message); }
    static void run() throws Exception {
        ByteBuffer file=ByteBuffer.wrap(Files.readAllBytes(Paths.get("/data/aac-fixture.bin"))).order(ByteOrder.LITTLE_ENDIAN);
        byte[] magic=new byte[8]; file.get(magic); check("STAAC001".equals(new String(magic,java.nio.charset.StandardCharsets.US_ASCII)),"AAC fixture version");
        int rate=file.getInt(),channels=file.getInt(),configSize=file.getInt(),count=file.getInt();
        check(rate==48000 && channels==2 && configSize>=2 && configSize<=64 && count>=40 && count<100,"AAC fixture bounds");
        byte[] config=new byte[configSize]; file.get(config); long[] times=new long[count]; byte[][] packets=new byte[count][];
        for(int i=0;i<count;i++) { times[i]=file.getLong(); int size=file.getInt(); check(size>0 && size<=file.remaining(),"AAC fixture packet"); packets[i]=new byte[size]; file.get(packets[i]); }
        check(!file.hasRemaining(),"AAC fixture trailing bytes");
        MediaFormat format=MediaFormat.createAudioFormat("audio/mp4a-latm",rate,channels); format.setByteBuffer("csd-0",ByteBuffer.wrap(config));
        MediaCodec codec=MediaCodec.createDecoderByType("audio/mp4a-latm");
        try {
            check("c2.stone.aac.decoder".equals(codec.getName()),"AAC decoder identity");
            codec.configure(format,null,null,0); codec.start();
            for(int pass=0;pass<2;pass++) {
                int sent=0,totalFrames=0,peak=0,positiveCrossings=0,previous=0; boolean inputEos=false,outputEos=false; long lastTime=Long.MIN_VALUE,end=System.nanoTime()+10000000000L;
                while(!outputEos && System.nanoTime()<end) {
                    int input=inputEos ? -1 : codec.dequeueInputBuffer(0);
                    if(input>=0) {
                        if(sent<count) { codec.getInputBuffer(input).put(packets[sent]); codec.queueInputBuffer(input,0,packets[sent].length,times[sent],0); sent++; }
                        else { inputEos=true; codec.queueInputBuffer(input,0,0,times[count-1]+21333,MediaCodec.BUFFER_FLAG_END_OF_STREAM); }
                    }
                    MediaCodec.BufferInfo info=new MediaCodec.BufferInfo(); int output;
                    while((output=codec.dequeueOutputBuffer(info,0))!=MediaCodec.INFO_TRY_AGAIN_LATER) {
                        if(output==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) { check(codec.getOutputFormat().getInteger("sample-rate")==rate && codec.getOutputFormat().getInteger("pcm-encoding")==2,"AAC output PCM format"); continue; }
                        check(output>=0 && info.size%4==0,"AAC output event/alignment");
                        if(info.size>0) {
                            check(info.presentationTimeUs>=lastTime,"AAC output timestamps went backwards"); lastTime=info.presentationTimeUs;
                            ByteBuffer pcm=codec.getOutputBuffer(output).order(ByteOrder.LITTLE_ENDIAN); pcm.position(info.offset).limit(info.offset+info.size);
                            while(pcm.remaining()>=4) { int left=pcm.getShort(),right=pcm.getShort(); peak=Math.max(peak,Math.max(Math.abs(left),Math.abs(right))); if(previous<=0 && left>0) positiveCrossings++; previous=left; totalFrames++; }
                        }
                        outputEos=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0; codec.releaseOutputBuffer(output,false);
                    }
                }
                double frequency=totalFrames==0 ? 0 : (double)positiveCrossings*rate/totalFrames;
                check(outputEos && totalFrames>=45000 && totalFrames<=60000 && peak>3000 && peak<12000 && Math.abs(frequency-440)<30,
                    "AAC actual 440 Hz PCM: frames="+totalFrames+" peak="+peak+" frequency="+frequency);
                if(pass==0) codec.flush();
            }
            codec.stop();
            MediaFormat unsupported=MediaFormat.createAudioFormat("audio/mp4a-latm",rate,channels); unsupported.setByteBuffer("csd-0",ByteBuffer.wrap(new byte[]{0x2b,(byte)0x90}));
            try { codec.configure(unsupported,null,null,0); throw new AssertionError("Unsupported AAC profile accepted"); } catch(IllegalArgumentException expected) {}
            System.out.println("PASS image AAC-LC: independent 440 Hz PCM frequency/amplitude, output format, timestamps, EOS, flush replay, stop and unsupported profile rejection");
        } finally { codec.release(); }
    }
}
