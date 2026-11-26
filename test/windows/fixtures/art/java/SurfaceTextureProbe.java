package com.trebledot.stone.probe;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.view.Surface;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Checks actual samplerExternalOES pixels and image EGL14 output, without a GPU decoder. */
final class SurfaceTextureProbe {
    private static final int EXTERNAL=0x8d65;
    private static void check(boolean value,String message) { if(!value) throw new AssertionError(message); }
    private static int shader(int kind,String source) {
        int shader=GLES20.glCreateShader(kind); GLES20.glShaderSource(shader,source); GLES20.glCompileShader(shader);
        int[] status={0}; GLES20.glGetShaderiv(shader,GLES20.GL_COMPILE_STATUS,status,0);
        check(status[0]!=0,"SurfaceTexture shader: "+GLES20.glGetShaderInfoLog(shader)); return shader;
    }
    private static void draw(int program,float[] matrix) {
        GLES20.glUseProgram(program); GLES20.glViewport(0,0,4,4);
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program,"transform"),1,false,matrix,0);
        GLES20.glUniform1i(GLES20.glGetUniformLocation(program,"image"),0);
        java.nio.FloatBuffer vertices=ByteBuffer.allocateDirect(32).order(ByteOrder.nativeOrder()).asFloatBuffer();
        vertices.put(new float[]{-1,-1,-1,1,1,-1,1,1}).flip();
        int location=GLES20.glGetAttribLocation(program,"p"); GLES20.glEnableVertexAttribArray(location);
        GLES20.glVertexAttribPointer(location,2,GLES20.GL_FLOAT,false,0,vertices);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4); GLES20.glDisableVertexAttribArray(location);
        check(GLES20.glGetError()==GLES20.GL_NO_ERROR,"SurfaceTexture external sampler GL error");
    }
    private static int texture() { int[] names={0}; GLES20.glGenTextures(1,names,0); GLES20.glBindTexture(EXTERNAL,names[0]); return names[0]; }
    private static void pixels(boolean second) {
        ByteBuffer pixels=ByteBuffer.allocateDirect(64);
        GLES20.glReadPixels(0,0,4,4,GLES20.GL_RGBA,GLES20.GL_UNSIGNED_BYTE,pixels);
        check((pixels.get(0)&255)==(second?255:0) && (pixels.get(1)&255)==(second?255:0) && (pixels.get(2)&255)==255,"SurfaceTexture bottom row/orientation");
        check((pixels.get(48)&255)==(second?0:255) && (pixels.get(49)&255)==(second?255:0) && (pixels.get(50)&255)==0,"SurfaceTexture top row/orientation");
    }
    static void run() throws Exception {
        EGLDisplay display=EGL14.eglGetDisplay(0);
        check(EGL14.eglInitialize(display,new int[1],0,new int[1],0),"SurfaceTexture EGL14 initialize");
        EGLConfig[] configs=new EGLConfig[1]; int[] count={0};
        check(EGL14.eglChooseConfig(display,new int[]{EGL14.EGL_SURFACE_TYPE,EGL14.EGL_PBUFFER_BIT|EGL14.EGL_WINDOW_BIT,
            EGL14.EGL_RENDERABLE_TYPE,EGL14.EGL_OPENGL_ES2_BIT,EGL14.EGL_RED_SIZE,8,EGL14.EGL_GREEN_SIZE,8,EGL14.EGL_BLUE_SIZE,8,EGL14.EGL_ALPHA_SIZE,8,EGL14.EGL_NONE},0,configs,0,1,count,0) && count[0]>0,"SurfaceTexture EGL config");
        EGLContext context=EGL14.eglCreateContext(display,configs[0],EGL14.EGL_NO_CONTEXT,new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION,2,EGL14.EGL_NONE},0);
        EGLSurface pbuffer=EGL14.eglCreatePbufferSurface(display,configs[0],new int[]{EGL14.EGL_WIDTH,4,EGL14.EGL_HEIGHT,4,EGL14.EGL_NONE},0);
        check(EGL14.eglMakeCurrent(display,pbuffer,pbuffer,context),"SurfaceTexture current EGL14 context");
        SurfaceTexture stream=new SurfaceTexture(texture()); Surface producer=new Surface(stream);
        Surface output=null; EGLSurface window=EGL14.EGL_NO_SURFACE;
        int program=0,vertex=0,fragment=0;
        try {
            stream.setDefaultBufferSize(4,4);
            int[] callbacks={0}; stream.setOnFrameAvailableListener(value->callbacks[0]++);
            Method render=Class.forName("com.trebledot.stone.runtime.ArtMediaCodecs").getDeclaredMethod("renderVideo",int[].class,int.class,int.class,Surface.class,long.class);
            render.setAccessible(true);
            vertex=shader(GLES20.GL_VERTEX_SHADER,"attribute vec2 p; varying vec2 uv; uniform mat4 transform; void main(){gl_Position=vec4(p,0.0,1.0); uv=(transform*vec4(p*0.5+0.5,0.0,1.0)).xy;}");
            fragment=shader(GLES20.GL_FRAGMENT_SHADER,"#extension GL_OES_EGL_image_external : require\nprecision mediump float; varying vec2 uv; uniform samplerExternalOES image; void main(){gl_FragColor=texture2D(image,uv);}");
            program=GLES20.glCreateProgram(); GLES20.glAttachShader(program,vertex); GLES20.glAttachShader(program,fragment); GLES20.glLinkProgram(program);
            int[] linked={0}; GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,linked,0); check(linked[0]!=0,"External sampler link");
            int[] activeSize={-1},activeType={-1};
            check("p".equals(GLES20.glGetActiveAttrib(program,0,activeSize,0,activeType,0))
                && activeSize[0]==1 && activeType[0]==GLES20.GL_FLOAT_VEC2,"Seven-argument GL attribute reflection ABI");
            java.util.HashSet<String> uniforms=new java.util.HashSet<>();
            for(int i=0;i<2;i++) uniforms.add(GLES20.glGetActiveUniform(program,i,activeSize,0,activeType,0));
            check(uniforms.contains("transform") && uniforms.contains("image"),"Seven-argument GL uniform reflection ABI");
            float[] matrix=new float[16];
            for(int pass=0;pass<2;pass++) {
                int[] rgba=new int[16]; java.util.Arrays.fill(rgba,0,8,pass==0?0xffff0000:0xff00ff00); java.util.Arrays.fill(rgba,8,16,pass==0?0xff0000ff:0xffffffff);
                render.invoke(null,rgba,4,4,producer,123000L+pass*1000);
                BroadcastProbe.drain(); check(callbacks[0]==pass+1,"SurfaceTexture real Handler frame notification");
                stream.updateTexImage(); check(stream.getTimestamp()==123000L+pass*1000,"SurfaceTexture exact buffer timestamp");
                stream.getTransformMatrix(matrix); draw(program,matrix); pixels(pass==1);
            }
            producer.release(); producer=new Surface(stream);
            check(producer.isValid() && !stream.isReleased(),"Surface release abandoned live SurfaceTexture");
            stream.detachFromGLContext();
            try { stream.updateTexImage(); throw new AssertionError("Detached SurfaceTexture updated"); } catch(IllegalStateException expected) {}
            stream.attachToGLContext(texture()); draw(program,matrix); pixels(true);
            Class<?> views=Class.forName("com.trebledot.stone.runtime.ArtSurfaceViews");
            Method create=views.getDeclaredMethod("createNative",Surface.class,int.class,int.class),read=views.getDeclaredMethod("pixelsNative",long.class);
            create.setAccessible(true); read.setAccessible(true);
            java.lang.reflect.Constructor<Surface> constructor=Surface.class.getDeclaredConstructor(); constructor.setAccessible(true);
            output=constructor.newInstance(); long token=(Long)create.invoke(null,output,4,4);
            window=EGL14.eglCreateWindowSurface(display,configs[0],output,new int[]{EGL14.EGL_NONE},0);
            check(!window.equals(EGL14.EGL_NO_SURFACE) && EGL14.eglMakeCurrent(display,window,window,context),"EGL14 software output window");
            draw(program,matrix);
            check(android.opengl.EGLExt.eglPresentationTimeANDROID(display,window,987654000L),"Image EGLExt output presentation timestamp");
            check(EGL14.eglSwapBuffers(display,window),"EGL14 software output swap");
            int[] shown=(int[])read.invoke(null,token);
            check(shown!=null && shown[0]==0xff00ff00 && shown[12]==0xffffffff,"External sampler pixels did not reach output Surface");
            stream.release(); check(stream.isReleased(),"SurfaceTexture release state");
            try { stream.updateTexImage(); throw new AssertionError("Released SurfaceTexture updated"); } catch(IllegalStateException expected) {}
            System.out.println("PASS image SurfaceTexture: actual external-OES shader pixels, vertical transform, second-frame upload, Handler notifications, exact timestamps, producer ownership, detach/attach, EGL14 output Surface and release errors");
        } finally {
            producer.release(); stream.release();
            if(program!=0) GLES20.glDeleteProgram(program); if(vertex!=0) GLES20.glDeleteShader(vertex); if(fragment!=0) GLES20.glDeleteShader(fragment);
            EGL14.eglMakeCurrent(display,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_CONTEXT);
            if(!window.equals(EGL14.EGL_NO_SURFACE)) EGL14.eglDestroySurface(display,window);
            if(output!=null) output.release();
            EGL14.eglDestroySurface(display,pbuffer); EGL14.eglDestroyContext(display,context); EGL14.eglTerminate(display);
        }
    }
}
