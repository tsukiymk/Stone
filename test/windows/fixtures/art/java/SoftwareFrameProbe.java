package com.trebledot.stone.probe;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.view.View;
import android.view.ViewGroup;
import java.lang.reflect.Method;

final class SoftwareFrameProbe {
    private static final class Tile extends View {
        int color = 0xffff0000, draws;
        boolean invalidateDuringDraw, fail;
        Tile(Activity activity) { super(activity); }
        @Override protected void onDraw(Canvas canvas) {
            draws++; canvas.drawColor(color);
            if (fail) { fail = false; throw new IllegalStateException("Software frame fixture"); }
            if (invalidateDuringDraw) { invalidateDuringDraw = false; color = 0xff00ff00; invalidate(); }
        }
    }
    private static void check(boolean value,String message) { if (!value) throw new AssertionError(message); }
    static void verify(Activity activity) throws Exception {
        Class<?> type = Class.forName("com.trebledot.stone.runtime.ArtViewRoot");
        Class<?> ripple = Class.forName("com.trebledot.stone.runtime.ArtSoftwareRipples");
        Method attach = type.getDeclaredMethod("attach",Activity.class), prepare = type.getDeclaredMethod("prepareFrame",int.class,int.class),
            draw = type.getDeclaredMethod("draw",Canvas.class,ripple,boolean.class);
        attach.setAccessible(true); prepare.setAccessible(true); draw.setAccessible(true);
        View decor = activity.getWindow().getDecorView();
        ViewGroup content = activity.findViewById(android.R.id.content);
        java.util.List<View> original = new java.util.ArrayList<>();
        java.util.List<Integer> visibility = new java.util.ArrayList<>();
        for (int i = 0; i < content.getChildCount(); ++i) { View child = content.getChildAt(i); original.add(child); visibility.add(child.getVisibility()); child.setVisibility(View.GONE); }
        Tile tile = new Tile(activity); content.addView(tile,new ViewGroup.LayoutParams(32,32));
        Bitmap bitmap = Bitmap.createBitmap(480,800,Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap); Object root = null;
        try {
            root = attach.invoke(null,activity);
            layout(decor,prepare,root,480,800);
            check((Boolean) draw.invoke(root,canvas,null,true),"First frame was not drawn");
            int[] point = new int[2]; tile.getLocationOnScreen(point);
            int x = point[0]+16, y = point[1]+16;
            check(bitmap.getPixel(x,y) == tile.color,"Initial frame pixels");
            int count = tile.draws;
            for (int i = 0; i < 4; ++i) check(!(Boolean) draw.invoke(root,canvas,null,false),"Clean frame was redrawn");
            check(tile.draws == count,"Idle view onDraw repeated");
            int[] notifications = {0};
            android.view.ViewTreeObserver.OnDrawListener observer = () -> {
                if (notifications[0]++ == 0) { tile.color = 0xff00ffff; tile.invalidate(); }
            };
            decor.getViewTreeObserver().addOnDrawListener(observer);
            try {
                check((Boolean) draw.invoke(root,canvas,null,false) && bitmap.getPixel(x,y) == 0xff00ffff,
                    "Draw observer damage was deferred or lost");
                count = tile.draws;
                check(!(Boolean) draw.invoke(root,canvas,null,false) && notifications[0] == 2 && tile.draws == count,
                    "Clean frame skipped draw observers or repainted unnecessarily");
            } finally { decor.getViewTreeObserver().removeOnDrawListener(observer); }
            tile.color = 0xff0000ff; tile.invalidate();
            check((Boolean) draw.invoke(root,canvas,null,false) && bitmap.getPixel(x,y) == tile.color,"Invalidated child remained stale");
            tile.invalidateDuringDraw = true; tile.invalidate(); draw.invoke(root,canvas,null,false);
            check(bitmap.getPixel(x,y) == 0xff0000ff,"Invalidation changed the in-flight frame");
            check((Boolean) draw.invoke(root,canvas,null,false) && bitmap.getPixel(x,y) == 0xff00ff00,"onDraw invalidation was lost");
            count = tile.draws; draw.invoke(root,canvas,null,true); check(tile.draws == count+1,"Forced redraw was cached");
            tile.fail = true; tile.invalidate();
            try { draw.invoke(root,canvas,null,false); throw new AssertionError("Draw failure was hidden"); }
            catch (java.lang.reflect.InvocationTargetException expected) { check(expected.getCause() instanceof IllegalStateException,"Unexpected drawing failure"); }
            tile.color = 0xffffff00;
            check((Boolean) draw.invoke(root,canvas,null,false) && bitmap.getPixel(x,y) == tile.color,"Failed layer was reused");
            java.lang.reflect.Field layer = type.getDeclaredField("layer"); layer.setAccessible(true);
            Bitmap previous = (Bitmap) layer.get(root);
            canvas.setBitmap(null); bitmap.recycle(); bitmap = Bitmap.createBitmap(480,801,Bitmap.Config.ARGB_8888); canvas.setBitmap(bitmap);
            layout(decor,prepare,root,480,801); draw.invoke(root,canvas,null,false);
            check(previous.isRecycled() && bitmap.getPixel(x,y) == tile.color,"Resize reused wrong layer");
            Bitmap retained = (Bitmap) layer.get(root);
            ((AutoCloseable) root).close(); root = null;
            check(retained.isRecycled(),"Detached root retained software pixels");
        } finally {
            try { if (root != null) ((AutoCloseable) root).close(); }
            finally {
                canvas.setBitmap(null); bitmap.recycle(); content.removeView(tile);
                for (int i = 0; i < original.size(); ++i) original.get(i).setVisibility(visibility.get(i));
            }
        }
        System.out.println("PASS software frame reuse, draw observers, child damage, in-draw invalidation, draw failure, resize and detach pixels.");
    }
    private static void layout(View decor,Method prepare,Object root,int width,int height) throws Exception {
        prepare.invoke(root,width,height);
        decor.measure(View.MeasureSpec.makeMeasureSpec(width,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(height,View.MeasureSpec.EXACTLY));
        decor.layout(0,0,width,height);
    }
}