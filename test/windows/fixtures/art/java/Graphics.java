package com.trebledot.stone.probe;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.LinearGradient;
import android.graphics.Shader;

public final class Graphics {
    private static native boolean verifyBitmap(Bitmap bitmap);
    private static native void unsupportedSurface(boolean toSurface);
    public static int raster() throws Exception {
        Bitmap bitmap = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
        try {
            Canvas canvas = new Canvas(bitmap);
            canvas.drawColor(Color.BLUE);
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
            paint.setColor(Color.RED);
            canvas.drawRect(8, 8, 32, 32, paint);
            canvas.save();
            canvas.translate(40, 40);
            paint.setColor(Color.GREEN);
            canvas.drawRect(0, 0, 16, 16, paint);
            canvas.restore();
            if (bitmap.getPixel(0, 0) != Color.BLUE || bitmap.getPixel(12, 12) != Color.RED
                    || bitmap.getPixel(48, 48) != Color.GREEN || bitmap.getPixel(36, 36) != Color.BLUE)
                throw new AssertionError("raster solid colors and transform");
            paint.setColor(0x80ff0000);
            canvas.drawRect(0, 32, 24, 64, paint);
            if (bitmap.getPixel(12, 48) != 0xff80007f)
                throw new AssertionError("raster alpha composition: " + Integer.toHexString(bitmap.getPixel(12, 48)));
            paint.setColor(Color.WHITE);
            canvas.drawRoundRect(32.25f, 0.25f, 63.75f, 31.75f, 6.5f, 6.5f, paint);
            if (bitmap.getPixel(48, 16) != Color.WHITE || bitmap.getPixel(32, 0) != Color.BLUE)
                throw new AssertionError("raster antialiased round rectangle");
            paint.setShader(new LinearGradient(0, 0, 64, 0, Color.BLACK, Color.WHITE, Shader.TileMode.CLAMP));
            canvas.drawRect(0, 0, 64, 8, paint);
            int previous = -1;
            for (int x = 0; x < 64; x++) {
                int pixel = bitmap.getPixel(x, 4);
                int level = Color.red(pixel);
                if (Color.alpha(pixel) != 255 || level != Color.green(pixel) || level != Color.blue(pixel)
                        || level <= previous || pixel != bitmap.getPixel(x, 6))
                    throw new AssertionError("raster grayscale gradient at " + x);
                previous = level;
            }
            paint.setShader(null);
            Class.forName("android.graphics.Typeface").getMethod("loadPreinstalledSystemFontMap").invoke(null);
            canvas.drawColor(Color.WHITE);
            paint.setColor(Color.BLACK);
            paint.setTextSize(20);
            canvas.drawText("Stone", 2, 28, paint);
            int ink = 0;
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                if (bitmap.getPixel(x, y) != Color.WHITE) {
                    if (y >= 40) throw new AssertionError("raster text outside glyph area");
                    ink++;
                }
            }
            if (ink < 40) throw new AssertionError("raster text has no visible glyphs");
            System.out.println("PASS image raster solid/alpha/transform/antialiased-round-rect/gradient/text pixels.");
            return 42;
        } finally { bitmap.recycle(); }
    }

    public static int run() {
        Bitmap bitmap = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(Color.BLUE);
        Paint paint = new Paint();
        paint.setColor(Color.RED);
        canvas.drawRect(8, 8, 32, 32, paint);
        canvas.save();
        canvas.translate(40, 40);
        paint.setColor(Color.GREEN);
        canvas.drawRect(0, 0, 16, 16, paint);
        canvas.restore();
        if (bitmap.getPixel(0, 0) != Color.BLUE || bitmap.getPixel(12, 12) != Color.RED
                || bitmap.getPixel(48, 48) != Color.GREEN || bitmap.getPixel(36, 36) != Color.BLUE)
            throw new AssertionError("native Canvas pixels");
        boolean nativeBitmap = verifyBitmap(bitmap);
        if (!nativeBitmap || bitmap.getPixel(0, 0) != 0xff345678 || bitmap.getPixel(12, 12) != Color.RED)
            throw new AssertionError("image NDK Bitmap info, lock and write/unlock: native=" + nativeBitmap
                + " pixel=" + Integer.toHexString(bitmap.getPixel(0, 0)));
        for (boolean toSurface : new boolean[]{false, true}) {
            try {
                unsupportedSurface(toSurface);
                throw new AssertionError("unsupported Surface conversion succeeded");
            } catch (UnsupportedOperationException expected) { }
        }
        bitmap.recycle();
        return 42;
    }
}
