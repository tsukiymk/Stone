package com.trebledot.stone.runtime;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import java.lang.reflect.Field;
import java.util.*;

/** CPU SurfaceView queues presented beneath the ordinary software view raster. */
final class ArtSurfaceViews implements AutoCloseable {
    private static native long createNative(Surface surface,int width,int height);
    private static native int[] pixelsNative(long token);
    private static native void stopNative(long token);
    private final Map<SurfaceView,Entry> entries = new IdentityHashMap<>();
    private static Field field(String name) throws ReflectiveOperationException {
        Field value = SurfaceView.class.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static final class Entry {
        SurfaceView view;
        long token;
        int width,height;
        Bitmap bitmap;
        Entry(SurfaceView view) { this.view=view; }
    }
    static void suppressCompositor(View view) throws ReflectiveOperationException {
        if (view instanceof SurfaceView) {
            SurfaceView surface = (SurfaceView)view;
            field("mHaveFrame").setBoolean(surface,false);
            ViewTreeObserver observer = surface.getViewTreeObserver();
            observer.removeOnPreDrawListener((ViewTreeObserver.OnPreDrawListener)field("mDrawListener").get(surface));
            observer.removeOnScrollChangedListener((ViewTreeObserver.OnScrollChangedListener)field("mScrollChangedListener").get(surface));
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup)view;
            for (int at=0;at<group.getChildCount();++at) suppressCompositor(group.getChildAt(at));
        }
    }
    @SuppressWarnings("unchecked")
    private static List<SurfaceHolder.Callback> callbacks(SurfaceView view) throws ReflectiveOperationException {
        return new ArrayList<>((List<SurfaceHolder.Callback>)field("mCallbacks").get(view));
    }
    private void destroy(Entry entry) throws ReflectiveOperationException {
        if (entry.token != 0) {
            // Wake a producer waiting for queue capacity before the renderer's
            // synchronous surfaceDestroyed callback waits for its raster thread.
            stopNative(entry.token);
            for (SurfaceHolder.Callback callback:callbacks(entry.view)) callback.surfaceDestroyed(entry.view.getHolder());
            field("mSurfaceCreated").setBoolean(entry.view,false);
            entry.view.getHolder().getSurface().release(); entry.token=0;
        }
        if (entry.bitmap != null) { entry.bitmap.recycle(); entry.bitmap=null; }
    }
    private void collect(View view,List<SurfaceView> result) {
        if (view instanceof SurfaceView && view.isAttachedToWindow() && view.isShown()) result.add((SurfaceView)view);
        if (view instanceof ViewGroup) {
            ViewGroup group=(ViewGroup)view;
            for(int at=0;at<group.getChildCount();++at) collect(group.getChildAt(at),result);
        }
    }
    boolean update(View root) throws ReflectiveOperationException {
        suppressCompositor(root);
        List<SurfaceView> visible=new ArrayList<>(); collect(root,visible);
        for (Iterator<Map.Entry<SurfaceView,Entry>> at=entries.entrySet().iterator();at.hasNext();) {
            Map.Entry<SurfaceView,Entry> item=at.next();
            if (!visible.contains(item.getKey())) { destroy(item.getValue()); at.remove(); }
        }
        boolean changed=false;
        for (SurfaceView view:visible) {
            int width=view.getWidth(),height=view.getHeight();
            int requestedWidth=field("mRequestedWidth").getInt(view),requestedHeight=field("mRequestedHeight").getInt(view);
            if(requestedWidth>0) width=requestedWidth;
            if(requestedHeight>0) height=requestedHeight;
            if(width<=0 || height<=0) continue;
            Entry entry=entries.get(view);
            if(entry==null) { entry=new Entry(view); entries.put(view,entry); }
            if(entry.token==0 || entry.width!=width || entry.height!=height) {
                destroy(entry);
                entry.width=width; entry.height=height;
                entry.token=createNative(view.getHolder().getSurface(),width,height);
                if(entry.token==0) throw new IllegalStateException("Cannot create software SurfaceView");
                ((Rect)field("mSurfaceFrame").get(view)).set(0,0,width,height);
                field("mSurfaceWidth").setInt(view,width); field("mSurfaceHeight").setInt(view,height);
                field("mSurfaceCreated").setBoolean(view,true);
                field("mIsCreating").setBoolean(view,true);
                try {
                    for(SurfaceHolder.Callback callback:callbacks(view)) callback.surfaceCreated(view.getHolder());
                    for(SurfaceHolder.Callback callback:callbacks(view)) callback.surfaceChanged(view.getHolder(),android.graphics.PixelFormat.RGBA_8888,width,height);
                } finally { field("mIsCreating").setBoolean(view,false); }
            }
            int[] pixels=pixelsNative(entry.token);
            if(pixels!=null) {
                if(entry.bitmap==null) entry.bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);
                entry.bitmap.setPixels(pixels,0,width,0,0,width,height);
                field("mDrawFinished").setBoolean(view,true);
                view.invalidate(); changed=true;
            }
        }
        return changed;
    }
    void draw(Canvas canvas) {
        int[] position=new int[2];
        for(Entry entry:entries.values()) if(entry.bitmap!=null && entry.view.isShown()) {
            entry.view.getLocationInWindow(position);
            canvas.drawBitmap(entry.bitmap,null,new Rect(position[0],position[1],position[0]+entry.view.getWidth(),position[1]+entry.view.getHeight()),null);
        }
    }
    @Override public void close() throws ReflectiveOperationException {
        for(Entry entry:entries.values()) destroy(entry);
        entries.clear();
    }
    void stopProducers() {
        // Compose can detach an AndroidView from an Activity pre-destroy
        // callback and synchronously join GLSurfaceView's producer thread.
        // Stop frame production before any such lifecycle callback.
        for(Entry entry:entries.values()) if(entry.token!=0) stopNative(entry.token);
    }
}
