package com.trebledot.stone.probe;

import android.media.*;
import android.os.Handler;
import android.os.HandlerThread;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** A local WebM fixture is decoded by the real image MediaCodec, then rendered. */
public final class AudioCodecProbe {
    private static void check(boolean v,String text) { if(!v) throw new AssertionError(text); }
    private static final class Packet { byte[] data; long pts; Packet(byte[] data,long pts) { this.data=data; this.pts=pts; } }
    private static final class Webm {
        byte[] data,head; int position; long cluster;
        final List<Packet> packets=new ArrayList<>();
        long vint(boolean id) {
            int first=data[position++]&255,mask=128,length=1;
            while((first&mask)==0 && length<8) { mask>>=1; length++; }
            long value=id ? first : first&(mask-1);
            for(int i=1;i<length;i++) value=(value<<8)|(data[position++]&255); return value;
        }
        void elements(int end) {
            while(position<end && packets.size()<120) {
                long id=vint(true),length=vint(false); int start=position;
                boolean master=id==0x18538067L || id==0x1654ae6b || id==0xae || id==0x1f43b675L;
                if(length<0 || (length>end-start && !master)) return; int limit=(int)(start+Math.min(length,end-start));
                if(master) elements(limit);
                else if(id==0x63a2) head=Arrays.copyOfRange(data,start,limit);
                else if(id==0xe7) { cluster=0; while(position<limit) cluster=(cluster<<8)|(data[position++]&255); }
                else if(id==0xa3) {
                    long track=vint(false); int time=(short)(((data[position++]&255)<<8)|(data[position++]&255)); int flags=data[position++]&255;
                    check(track==1 && (flags&6)==0,"single-track non-laced Opus fixture");
                    packets.add(new Packet(Arrays.copyOfRange(data,position,limit),(cluster+time)*1000));
                }
                position=limit;
            }
        }
        Webm() throws Exception { data=Files.readAllBytes(Paths.get("/data/opus-fixture.webm")); elements(data.length); check(head!=null && packets.size()>30,"Opus WebM fixture packets"); }
        MediaFormat format() {
            MediaFormat format=MediaFormat.createAudioFormat("audio/opus",48000,head[9]&255);
            format.setByteBuffer("csd-0",ByteBuffer.wrap(head)); return format;
        }
    }
    public static void run() throws Exception {
        Webm fixture=new Webm();
        MediaCodec codec=MediaCodec.createDecoderByType("audio/opus");
        long bytes=0,peak=0; int channels=fixture.head[9]&255;
        AudioTrack track=new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build())
            .setAudioFormat(new AudioFormat.Builder().setEncoding(2).setSampleRate(48000).setChannelMask(channels==1 ? 4 : 12).build())
            .setBufferSizeInBytes(19200).build();
        try {
            codec.configure(fixture.format(),null,null,0); codec.start(); track.play();
            int next=0; boolean ended=false,sent=false; MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            while(!ended) {
                int input=sent ? -1 : codec.dequeueInputBuffer(10000);
                if(input>=0) {
                    Packet p=next<fixture.packets.size() ? fixture.packets.get(next++) : null;
                    ByteBuffer buffer=codec.getInputBuffer(input); buffer.clear(); if(p!=null) buffer.put(p.data);
                    codec.queueInputBuffer(input,0,p==null ? 0 : p.data.length,p==null ? 2400000 : p.pts,p==null ? 4 : 0); sent=p==null;
                }
                int out=codec.dequeueOutputBuffer(info,10000);
                if(out>=0) {
                    ByteBuffer pcm=codec.getOutputBuffer(out).order(ByteOrder.nativeOrder()); pcm.position(info.offset).limit(info.offset+info.size);
                    ByteBuffer samples=pcm.duplicate().order(ByteOrder.nativeOrder()); while(samples.remaining()>=2) peak=Math.max(peak,Math.abs((int)samples.getShort()));
                    bytes+=info.size; check(track.write(pcm,info.size,AudioTrack.WRITE_BLOCKING)==info.size,"decoded PCM write");
                    ended=(info.flags&4)!=0; codec.releaseOutputBuffer(out,false);
                }
            }
            check(bytes>48000 && peak>100,"actual non-silent Opus PCM"); Thread.sleep(180);
            check(track.getPlaybackHeadPosition()>10000,"decoded Opus reached WASAPI");
            codec.flush();
            int input=codec.dequeueInputBuffer(0); check(input>=0,"input ownership after flush");
            Packet first=fixture.packets.get(0); codec.getInputBuffer(input).put(first.data);
            codec.queueInputBuffer(input,0,first.data.length,1000000,0);
            int output=codec.dequeueOutputBuffer(info,10000); if(output==-2) output=codec.dequeueOutputBuffer(info,10000);
            check(output>=0 && info.size>0,"decoded output after flush");
            System.out.println("Opus after flush first packet bytes="+info.size+" pts="+info.presentationTimeUs);
            codec.releaseOutputBuffer(output,false);
            codec.stop(); codec.configure(fixture.format(),null,null,0); codec.start(); codec.stop();
            System.out.println("PASS synchronous Opus decode bytes="+bytes+" peak="+peak+", EOS, flush, stop/reconfigure and real AudioTrack playback");
        } finally { codec.release(); track.release(); }
        asynchronous(fixture);
    }
    private static void asynchronous(Webm fixture) throws Exception {
        HandlerThread thread=new HandlerThread("codec-callback-test"); thread.start();
        MediaCodec codec=MediaCodec.createDecoderByType("audio/opus");
        AtomicReference<CountDownLatch> ended=new AtomicReference<>(new CountDownLatch(1)); AtomicReference<Throwable> failure=new AtomicReference<>(); AtomicInteger next=new AtomicInteger(),formats=new AtomicInteger(),bytes=new AtomicInteger();
        ArrayDeque<Integer> held=new ArrayDeque<>();
        codec.setCallback(new MediaCodec.Callback() {
            public void onInputBufferAvailable(MediaCodec source,int index) {
                try { int i=next.getAndIncrement(); if(i>fixture.packets.size()) return; Packet p=i<fixture.packets.size() ? fixture.packets.get(i) : null;
                    ByteBuffer input=source.getInputBuffer(index); input.clear(); if(p!=null) input.put(p.data);
                    source.queueInputBuffer(index,0,p==null ? 0 : p.data.length,p==null ? 2400000 : p.pts,p==null ? 4 : 0);
                } catch(Throwable error) { failure.set(error); ended.get().countDown(); }
            }
            public void onOutputBufferAvailable(MediaCodec source,int index,MediaCodec.BufferInfo info) {
                try { check(formats.get()>0,"format before asynchronous PCM"); check(source.getOutputBuffer(index).remaining()==info.size,"async output byte range"); bytes.addAndGet(info.size); held.add(index);
                    if(held.size()==8 || (info.flags&4)!=0) while(!held.isEmpty()) source.releaseOutputBuffer(held.removeFirst(),false);
                    if((info.flags&4)!=0) ended.get().countDown(); }
                catch(Throwable error) { failure.set(error); ended.get().countDown(); }
            }
            public void onOutputFormatChanged(MediaCodec source,MediaFormat format) { formats.incrementAndGet(); }
            public void onError(MediaCodec source,MediaCodec.CodecException error) { failure.set(error); ended.get().countDown(); }
        },new Handler(thread.getLooper()));
        try {
            codec.configure(fixture.format(),null,null,MediaCodec.CONFIGURE_FLAG_USE_CRYPTO_ASYNC); codec.start();
            for(int pass=0;pass<2;pass++) {
                check(ended.get().await(15,TimeUnit.SECONDS),"asynchronous Opus EOS timeout");
                if(failure.get()!=null) throw new AssertionError("asynchronous codec callback",failure.get());
                check(bytes.get()>48000 && formats.get()==1,"asynchronous decoded PCM and format");
                if(pass==0) { codec.flush(); next.set(0); formats.set(0); bytes.set(0); ended.set(new CountDownLatch(1)); codec.start(); }
            }
            System.out.println("PASS asynchronous image MediaCodec input/output callbacks, PCM, EOS and flush/restart");
        } finally { codec.release(); thread.quitSafely(); thread.join(3000); }
    }
}
