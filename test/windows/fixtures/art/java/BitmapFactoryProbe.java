package com.trebledot.stone.probe;

import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.ColorSpace;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;

final class BitmapFactoryProbe {
    private static BitmapFactory.Options hardware() {
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inPreferredConfig = Bitmap.Config.HARDWARE;
        options.inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.SRGB);
        options.inSampleSize = 2;
        return options;
    }
    private static void verify(Bitmap bitmap, BitmapFactory.Options options) {
        if (bitmap == null || bitmap.getConfig() != Bitmap.Config.ARGB_8888 || bitmap.isMutable()
                || bitmap.getWidth() != 4 || bitmap.getHeight() != 2 || bitmap.getPixel(1,1) != Color.RED
                || options.inPreferredConfig != Bitmap.Config.HARDWARE || options.inSampleSize != 2
                || options.outWidth != 4 || options.outHeight != 2 || !"image/png".equals(options.outMimeType)
                || options.outConfig != bitmap.getConfig() || options.outColorSpace == null)
            throw new AssertionError("Image BitmapFactory did not preserve pixels, sampling, metadata and caller inputs");
        Bitmap target = Bitmap.createBitmap(4,2,Bitmap.Config.ARGB_8888);
        android.graphics.Canvas canvas = new android.graphics.Canvas(target);
        canvas.drawBitmap(bitmap,0,0,null);
        if (target.getPixel(1,1) != Color.RED) throw new AssertionError("Decoded bitmap cannot be drawn on software Canvas");
        canvas.setBitmap(null); target.recycle(); bitmap.recycle();
    }
    static void verify(AssetManager assets) throws Exception {
        Bitmap source = Bitmap.createBitmap(8,4,Bitmap.Config.ARGB_8888); source.eraseColor(Color.RED);
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        if (!source.compress(Bitmap.CompressFormat.PNG,100,output)) throw new AssertionError("PNG fixture encoding");
        source.recycle(); byte[] bytes = output.toByteArray();
        android.graphics.BitmapRegionDecoder region = android.graphics.BitmapRegionDecoder.newInstance(
            new ByteArrayInputStream(bytes), false);
        try {
            Bitmap tile = region.decodeRegion(new android.graphics.Rect(2,0,6,4), null);
            if (tile == null || tile.getWidth() != 4 || tile.getHeight() != 4 || tile.getPixel(1,1) != Color.RED)
                throw new AssertionError("Image BitmapRegionDecoder region pixels");
            tile.recycle();
        } finally { region.recycle(); }
        BitmapFactory.Options options = hardware();
        verify(BitmapFactory.decodeByteArray(bytes,0,bytes.length,options),options);
        options = hardware();
        verify(BitmapFactory.decodeStream(new ByteArrayInputStream(bytes),null,options),options);
        File file = new File("/data/bitmap-factory-probe.png");
        try {
            try (FileOutputStream stream = new FileOutputStream(file)) { stream.write(bytes); }
            try (FileInputStream stream = new FileInputStream(file)) {
                options = hardware(); verify(BitmapFactory.decodeFileDescriptor(stream.getFD(),null,options),options);
            }
        } finally { if (!file.delete()) throw new AssertionError("Bitmap fixture cleanup"); }
        if (assets != null) try (java.io.InputStream stream = assets.open("bitmap-check.png")) {
            options = hardware(); verify(BitmapFactory.decodeStream(stream,null,options),options);
        }
        options = hardware(); options.inJustDecodeBounds = true;
        BitmapFactory.Options reference = new BitmapFactory.Options(); reference.inSampleSize = 2; reference.inJustDecodeBounds = true;
        if (BitmapFactory.decodeByteArray(bytes,0,bytes.length,reference) != null) throw new AssertionError("Software bounds-only reference");
        if (BitmapFactory.decodeByteArray(bytes,0,bytes.length,options) != null || options.outWidth != reference.outWidth || options.outHeight != reference.outHeight
                || options.inPreferredConfig != Bitmap.Config.HARDWARE) throw new AssertionError("Bounds-only decode semantics");
        options = hardware();
        if (BitmapFactory.decodeByteArray(new byte[]{1,2,3},0,3,options) != null || options.outWidth != -1
                || options.outHeight != -1 || options.inPreferredConfig != Bitmap.Config.HARDWARE)
            throw new AssertionError("Invalid encoded image semantics");
        options = hardware(); options.inMutable = true;
        try { BitmapFactory.decodeByteArray(bytes,0,bytes.length,options); throw new AssertionError("Invalid mutable hardware options accepted"); }
        catch (IllegalArgumentException expected) { }
        System.out.println("PASS image BitmapFactory: hardware requests use drawable software storage; byte[], stream, descriptor"
            + (assets == null ? "" : ", asset") + "; exact pixels, sample, bounds, metadata, input isolation and invalid options.");
    }
}
