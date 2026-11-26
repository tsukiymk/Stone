package com.trebledot.stone.cit.gles;

import android.app.Activity;
import android.graphics.Color;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.widget.FrameLayout;
import android.widget.TextView;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public final class GlesActivity extends Activity implements GLSurfaceView.Renderer {
    private int width;
    private int height;
    private boolean painted;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        FrameLayout content = new FrameLayout(this);
        GLSurfaceView gl = new GLSurfaceView(this);
        gl.setEGLContextClientVersion(2);
        gl.setRenderer(this);
        gl.setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
        content.addView(gl, new FrameLayout.LayoutParams(-1, -1));
        TextView label = new TextView(this);
        label.setText("Canvas over OpenGL ES");
        label.setBackgroundColor(Color.GREEN);
        content.addView(label, new FrameLayout.LayoutParams(-1, 64, Gravity.TOP));
        setContentView(content);
    }

    @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        String version = GLES20.glGetString(GLES20.GL_VERSION);
        if (version == null || !version.startsWith("OpenGL ES")) {
            throw new AssertionError("Expected an OpenGL ES context");
        }
        Log.i("WindowsGles", version);
    }

    @Override public void onSurfaceChanged(GL10 gl, int width, int height) {
        this.width = width;
        this.height = height;
        GLES20.glViewport(0, 0, width, height);
    }

    @Override public void onDrawFrame(GL10 gl) {
        GLES20.glClearColor(1, 0, 0, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
        GLES20.glScissor(0, 0, width, height / 2);
        GLES20.glClearColor(0, 0, 1, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
        if (GLES20.glGetError() != GLES20.GL_NO_ERROR) throw new AssertionError("OpenGL ES rendering failed");
        if (!painted) {
            Log.i("WindowsGles", "Red and blue rectangles drawn");
            painted = true;
        }
    }
}
