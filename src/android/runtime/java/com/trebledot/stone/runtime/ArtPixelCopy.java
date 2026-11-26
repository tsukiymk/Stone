package com.trebledot.stone.runtime;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.view.PixelCopy;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/** Copies the last submitted software Surface frame using the image's Bitmap/Canvas. */
final class ArtPixelCopy {
    static void copy(int[] frame,int left,int top,int right,int bottom,Object request) throws Exception {
        Class<?> type=Class.forName("android.graphics.HardwareRenderer$CopyRequest");
        Method finished=type.getMethod("onCopyFinished",int.class);
        int result=PixelCopy.ERROR_SOURCE_NO_DATA;
        if(frame!=null && frame.length>2) {
            int width=frame[0],height=frame[1];
            Rect source=new Rect(left,top,right,bottom);
            if(source.isEmpty()) source.set(0,0,width,height);
            if(!source.intersect(0,0,width,height)) result=PixelCopy.ERROR_SOURCE_INVALID;
            else {
                Method allocate=type.getMethod("getDestinationBitmap",int.class,int.class);
                allocate.invoke(request,source.width(),source.height());
                Field field=type.getDeclaredField("mDestinationBitmap"); field.setAccessible(true);
                Bitmap destination=(Bitmap)field.get(request);
                if(destination==null || destination.isRecycled() || !destination.isMutable()) result=PixelCopy.ERROR_DESTINATION_INVALID;
                else {
                    Bitmap bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);
                    try {
                        bitmap.setPixels(frame,2,width,0,0,width,height);
                        Canvas canvas=new Canvas(destination);
                        try { canvas.drawBitmap(bitmap,source,new Rect(0,0,destination.getWidth(),destination.getHeight()),new Paint(Paint.FILTER_BITMAP_FLAG)); }
                        finally { canvas.setBitmap(null); }
                        result=PixelCopy.SUCCESS;
                    } finally { bitmap.recycle(); }
                }
            }
        }
        finished.invoke(request,result);
    }
}
