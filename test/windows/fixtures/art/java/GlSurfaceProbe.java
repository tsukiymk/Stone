package com.trebledot.stone.probe;

import android.opengl.GLES20;
import android.view.Surface;
import javax.microedition.khronos.egl.*;
import java.lang.reflect.Method;

/** Real image EGL/GLES JNI, ANGLE rendering and software queue readback. */
final class GlSurfaceProbe {
    private static native boolean swapInterval(int interval);
    private static int copy(Surface surface,android.graphics.Rect crop,android.graphics.Bitmap destination) throws Exception {
        android.os.HandlerThread thread=new android.os.HandlerThread("PixelCopy callback"); thread.start();
        java.util.concurrent.CountDownLatch done=new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.atomic.AtomicInteger status=new java.util.concurrent.atomic.AtomicInteger(-1);
        try {
            android.view.PixelCopy.request(surface,crop,destination,value->{status.set(value);done.countDown();},new android.os.Handler(thread.getLooper()));
            check(done.await(2,java.util.concurrent.TimeUnit.SECONDS),"PixelCopy callback missing"); return status.get();
        } finally { thread.quitSafely(); thread.join(2000); check(!thread.isAlive(),"PixelCopy callback thread did not stop"); }
    }
    private static void check(boolean value,String text) { if(!value) throw new AssertionError(text); }
    static void frameRateHints() throws Exception {
        Class<?> views=Class.forName("com.trebledot.stone.runtime.ArtSurfaceViews");
        Method create=views.getDeclaredMethod("createNative",Surface.class,int.class,int.class);
        Method frameRate=Surface.class.getDeclaredMethod("nativeSetFrameRate",long.class,float.class,int.class,int.class);
        create.setAccessible(true); frameRate.setAccessible(true);
        java.lang.reflect.Constructor<Surface> constructor=Surface.class.getDeclaredConstructor(); constructor.setAccessible(true);
        Surface surface=constructor.newInstance();
        try {
            long token=(Long)create.invoke(null,surface,4,4);
            check(token!=0 && surface.isValid(),"Software Surface creation");
            for(float rate:new float[]{0,24,60}) {
                check((Integer)frameRate.invoke(null,token,rate,1,0)==(rate==0 ? 0 : -38),"Software frame-rate vote/clear status");
                surface.setFrameRate(rate,Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            }
            surface.clearFrameRate();
            for(float rate:new float[]{-1,Float.NaN,Float.POSITIVE_INFINITY}) {
                try { surface.setFrameRate(rate,Surface.FRAME_RATE_COMPATIBILITY_DEFAULT); throw new AssertionError("Invalid frame rate accepted"); }
                catch(IllegalArgumentException expected) { }
            }
            check((Integer)frameRate.invoke(null,token,24f,3,0)==-22,"Invalid frame-rate compatibility");
            check((Integer)frameRate.invoke(null,token,24f,1,2)==-22,"Invalid frame-rate strategy");
        } finally { surface.release(); }
        check(!surface.isValid(),"Released software Surface remains valid");
        try { surface.setFrameRate(24,Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE); throw new AssertionError("Released Surface accepted frame-rate call"); }
        catch(IllegalStateException expected) { }
        System.out.println("PASS software Surface frame-rate JNI: unsupported status, invalid arguments and release without image handle dereference.");
    }
    static void run(android.content.Context context) throws Exception {
        int version=context.getSystemService(android.app.ActivityManager.class).getDeviceConfigurationInfo().reqGlEsVersion;
        check(version>=0x20000,"No real host OpenGL ES 2 context available");
        Class<?> views=Class.forName("com.trebledot.stone.runtime.ArtSurfaceViews");
        Method create=views.getDeclaredMethod("createNative",Surface.class,int.class,int.class), pixels=views.getDeclaredMethod("pixelsNative",long.class);
        create.setAccessible(true); pixels.setAccessible(true);
        java.lang.reflect.Constructor<Surface> constructor=Surface.class.getDeclaredConstructor(); constructor.setAccessible(true);
        Surface nativeSurface=constructor.newInstance(); long token=(Long)create.invoke(null,nativeSurface,4,4);
        EGL10 egl=(EGL10)EGLContext.getEGL(); EGLDisplay display=egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
        EGLContext gl=EGL10.EGL_NO_CONTEXT; EGLSurface surface=EGL10.EGL_NO_SURFACE;
        check(egl.eglInitialize(display,new int[2]),"Image EGL display initialization");
        try {
            EGLConfig[] configs=new EGLConfig[1]; int[] count=new int[1];
            check(egl.eglChooseConfig(display,new int[]{EGL10.EGL_SURFACE_TYPE,EGL10.EGL_WINDOW_BIT,0x3040,4,
                EGL10.EGL_RED_SIZE,8,EGL10.EGL_GREEN_SIZE,8,EGL10.EGL_BLUE_SIZE,8,EGL10.EGL_ALPHA_SIZE,8,EGL10.EGL_NONE},configs,1,count)
                && count[0]>0,"Image EGL window config");
            gl=egl.eglCreateContext(display,configs[0],EGL10.EGL_NO_CONTEXT,new int[]{0x3098,2,EGL10.EGL_NONE});
            check(gl!=EGL10.EGL_NO_CONTEXT,"Image EGL ES2 context");
            surface=egl.eglCreateWindowSurface(display,configs[0],nativeSurface,null);
            check(surface!=EGL10.EGL_NO_SURFACE && egl.eglMakeCurrent(display,surface,surface,gl),"Software Surface EGL attachment");
            check(GLES20.glGetString(GLES20.GL_VERSION)!=null,"Image GLES version query");
            GLES20.glViewport(0,0,4,4);
            android.graphics.Bitmap screenshot=android.graphics.Bitmap.createBitmap(2,2,android.graphics.Bitmap.Config.ARGB_8888);
            check(copy(nativeSurface,null,screenshot)==android.view.PixelCopy.ERROR_SOURCE_NO_DATA,"Empty software Surface PixelCopy");
            for(int color:new int[]{0xffff0000,0xff00ff00}) {
                GLES20.glClearColor((color>>>16&255)/255f,(color>>>8&255)/255f,(color&255)/255f,1);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                check(GLES20.glGetError()==GLES20.GL_NO_ERROR && egl.eglSwapBuffers(display,surface),"Image GLES clear/swap");
                int[] frame=(int[])pixels.invoke(null,token);
                check(frame!=null && frame.length==16,"Software EGL queue frame size");
                for(int pixel:frame) check(pixel==color,"Actual GLES pixels changed");
                check(copy(nativeSurface,null,screenshot)==android.view.PixelCopy.SUCCESS,"PixelCopy of a consumed software frame");
                for(int y=0;y<2;y++) for(int x=0;x<2;x++) check(screenshot.getPixel(x,y)==color,"PixelCopy scaled frame pixels");
            }
            GLES20.glEnable(GLES20.GL_SCISSOR_TEST); GLES20.glScissor(0,0,2,4); GLES20.glClearColor(1,0,0,1); GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT); GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
            check(egl.eglSwapBuffers(display,surface),"Crop source swap");
            check(copy(nativeSurface,new android.graphics.Rect(0,0,2,4),screenshot)==android.view.PixelCopy.SUCCESS,"Cropped software PixelCopy");
            for(int y=0;y<2;y++) for(int x=0;x<2;x++) check(screenshot.getPixel(x,y)==0xffff0000,"Cropped PixelCopy pixels");
            check(pixels.invoke(null,token)!=null,"PixelCopy consumed the display queue");
            screenshot.recycle();
            System.out.println("PASS real PixelCopy no-data error, callback Handler, cached/queued frames, crop/scale pixels and display queue preservation");
            long paced=System.nanoTime();
            for(int i=0;i<30;i++) check(egl.eglSwapBuffers(display,surface),"Paced screen swap without a UI consumer");
            long pacedMs=(System.nanoTime()-paced)/1000000;
            check(pacedMs>=430 && pacedMs<2000,"Screen refresh did not pace at 60 Hz: "+pacedMs);
            check(swapInterval(0),"Disable screen swap interval");
            long unpaced=System.nanoTime();
            for(int i=0;i<30;i++) check(egl.eglSwapBuffers(display,surface),"Unpaced screen swap");
            long unpacedMs=(System.nanoTime()-unpaced)/1000000;
            check(unpacedMs<pacedMs/2,"Zero swap interval still paced: "+unpacedMs);
            check(swapInterval(1),"Restore screen swap interval");
            Thread.sleep(250);
            long resumed=System.nanoTime();
            for(int i=0;i<4;i++) check(egl.eglSwapBuffers(display,surface),"Resumed screen swap");
            check((System.nanoTime()-resumed)/1000000>=45,"Paused screen caught up with a frame burst");
            check(((android.view.WindowManager)context.getSystemService(android.content.Context.WINDOW_SERVICE)).getDefaultDisplay().getRefreshRate()==60f,"Display and EGL screen refresh differ");
            System.out.println("PASS image EGL10/GLES20 real ANGLE context, software Surface, two rendered colors, 60 Hz pacing without UI consumption ("+pacedMs+" ms/30 swaps), interval zero ("+unpacedMs+" ms), resume without catch-up; ES capability="+Integer.toHexString(version));
        } finally {
            egl.eglMakeCurrent(display,EGL10.EGL_NO_SURFACE,EGL10.EGL_NO_SURFACE,EGL10.EGL_NO_CONTEXT);
            if(surface!=EGL10.EGL_NO_SURFACE) egl.eglDestroySurface(display,surface);
            if(gl!=EGL10.EGL_NO_CONTEXT) egl.eglDestroyContext(display,gl);
            egl.eglTerminate(display); nativeSurface.release();
        }
    }
    static void queuedUiOperations(com.trebledot.stone.runtime.ArtApplication app) throws Exception {
        android.app.Activity activity=app.startActivity();
        android.opengl.GLSurfaceView view=new android.opengl.GLSurfaceView(activity);
        view.setEGLContextClientVersion(2);
        java.util.concurrent.atomic.AtomicInteger frames=new java.util.concurrent.atomic.AtomicInteger();
        view.setRenderer(new android.opengl.GLSurfaceView.Renderer() {
            public void onSurfaceCreated(javax.microedition.khronos.opengles.GL10 gl,EGLConfig config) {}
            public void onSurfaceChanged(javax.microedition.khronos.opengles.GL10 gl,int width,int height) { GLES20.glViewport(0,0,width,height); }
            public void onDrawFrame(javax.microedition.khronos.opengles.GL10 gl) {
                GLES20.glClearColor(0,0,1,1); GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT); frames.incrementAndGet();
            }
        });
        android.widget.FrameLayout content=new android.widget.FrameLayout(activity); content.addView(view); activity.setContentView(content);
        android.app.Application.ActivityLifecycleCallbacks callback=new android.app.Application.ActivityLifecycleCallbacks() {
            public void onActivityPreDestroyed(android.app.Activity target) { if(target==activity) content.removeAllViews(); }
            public void onActivityCreated(android.app.Activity target,android.os.Bundle saved) {}
            public void onActivityStarted(android.app.Activity target) {}
            public void onActivityResumed(android.app.Activity target) {}
            public void onActivityPaused(android.app.Activity target) {}
            public void onActivityStopped(android.app.Activity target) {}
            public void onActivitySaveInstanceState(android.app.Activity target,android.os.Bundle saved) {}
            public void onActivityDestroyed(android.app.Activity target) {}
        };
        app.application.registerActivityLifecycleCallbacks(callback);
        Object root=com.trebledot.stone.runtime.ArtWindow.attach(activity);
        Method prepare=root.getClass().getDeclaredMethod("prepareFrame",int.class,int.class);
        Method draw=root.getClass().getDeclaredMethod("draw",android.graphics.Canvas.class,Class.forName("com.trebledot.stone.runtime.ArtSoftwareRipples"),boolean.class);
        prepare.setAccessible(true); draw.setAccessible(true);
        android.graphics.Bitmap bitmap=android.graphics.Bitmap.createBitmap(32,32,android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas canvas=new android.graphics.Canvas(bitmap);
        try {
            android.view.View decor=activity.getWindow().getDecorView(); prepare.invoke(root,32,32);
            decor.measure(android.view.View.MeasureSpec.makeMeasureSpec(32,1073741824),android.view.View.MeasureSpec.makeMeasureSpec(32,1073741824)); decor.layout(0,0,32,32);
            draw.invoke(root,canvas,null,true);
            long deadline=android.os.SystemClock.uptimeMillis()+5000;
            while(frames.get()<8 && android.os.SystemClock.uptimeMillis()<deadline) Thread.sleep(10);
            check(frames.get()>=8,"Screen producer blocked while UI thread was not consuming frames");
            java.util.concurrent.CountDownLatch event=new java.util.concurrent.CountDownLatch(1);
            view.queueEvent(event::countDown);
            check(event.await(2,java.util.concurrent.TimeUnit.SECONDS),"UI thread waiting for a queued GL event deadlocked");
            long paused=android.os.SystemClock.uptimeMillis(); view.onPause(); view.onResume();
            check(android.os.SystemClock.uptimeMillis()-paused<3000,"GL pause/resume blocked without a UI frame consumer");
            java.lang.reflect.Field field=android.opengl.GLSurfaceView.class.getDeclaredField("mGLThread"); field.setAccessible(true);
            Thread producer=(Thread)field.get(view);
            long started=android.os.SystemClock.uptimeMillis(); app.destroyActivity();
            check(android.os.SystemClock.uptimeMillis()-started<3000 && !producer.isAlive(),"Activity pre-destroy GL thread join stalled");
            System.out.println("PASS actual GLSurfaceView continues beyond screen queue capacity, synchronous UI/GL event, pause/resume and Activity pre-destroy thread join");
        } finally {
            app.application.unregisterActivityLifecycleCallbacks(callback);
            ((AutoCloseable)root).close(); canvas.setBitmap(null); bitmap.recycle(); app.destroyActivity();
        }
    }
}
