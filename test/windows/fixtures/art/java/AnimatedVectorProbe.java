package com.trebledot.stone.probe;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.AnimatedStateListDrawable;
import android.graphics.drawable.AnimatedVectorDrawable;
import android.graphics.drawable.Drawable;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.widget.CheckedTextView;

/** Inflates and draws the selected image's actual animated single-choice indicator. */
final class AnimatedVectorProbe {
    static void verify(Context context) {
        Context themed = new ContextThemeWrapper(context,android.R.style.Theme_DeviceDefault_DayNight);
        CheckedTextView view = (CheckedTextView) LayoutInflater.from(themed).inflate(android.R.layout.simple_list_item_single_choice,null,false);
        Drawable mark = view.getCheckMarkDrawable();
        if (!(mark instanceof AnimatedStateListDrawable)) throw new AssertionError("Image single-choice indicator is not an animated state list");
        mark.setBounds(0,0,64,64);
        Bitmap off = Bitmap.createBitmap(64,64,Bitmap.Config.ARGB_8888), on = Bitmap.createBitmap(64,64,Bitmap.Config.ARGB_8888);
        try {
            mark.setState(new int[]{android.R.attr.state_enabled}); mark.jumpToCurrentState(); draw(mark,off);
            mark.setState(new int[]{android.R.attr.state_enabled,android.R.attr.state_checked});
            if (!(mark.getCurrent() instanceof AnimatedVectorDrawable)) throw new AssertionError("Image checked transition did not create the real animated vector");
            draw(mark,on); // Exercise the image's software-canvas fallback, not a hardware renderer.
            mark.jumpToCurrentState(); draw(mark,on);
            if (off.sameAs(on)) throw new AssertionError("Image animated radio transition did not produce distinct checked pixels");
            System.out.println("PASS image animated single-choice XML inflation, real vector transition, software draw and distinct final checked pixels.");
        } finally { mark.setCallback(null); off.recycle(); on.recycle(); }
    }
    private static void draw(Drawable drawable, Bitmap bitmap) {
        bitmap.eraseColor(0xff888888); Canvas canvas = new Canvas(bitmap);
        try { drawable.draw(canvas); } finally { canvas.setBitmap(null); }
    }
}
