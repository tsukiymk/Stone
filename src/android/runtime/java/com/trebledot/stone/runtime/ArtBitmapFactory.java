package com.trebledot.stone.runtime;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory.Options;
import android.graphics.Rect;
import java.io.FileDescriptor;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

/** Keep image decoding in libhwui while selecting storage the software window can draw. */
final class ArtBitmapFactory {
    private static final Field[] FIELDS = Options.class.getFields();

    private static Options software(Options original) {
        if (original == null || original.inPreferredConfig != Bitmap.Config.HARDWARE) return original;
        Options copy = new Options();
        try {
            // Includes deprecated/platform inputs as well as density, reuse,
            // sampling and color space. Never mutate the caller's input options.
            for (Field field : FIELDS) if (!Modifier.isStatic(field.getModifiers())) field.set(copy,field.get(original));
        } catch (IllegalAccessException error) { throw new AssertionError(error); }
        copy.inPreferredConfig = Bitmap.Config.ARGB_8888;
        return copy;
    }

    private static void outputs(Options original, Options decoded) {
        if (original == decoded) return;
        try {
            for (Field field : FIELDS) if (!Modifier.isStatic(field.getModifiers()) && field.getName().startsWith("out"))
                field.set(original,field.get(decoded));
        } catch (IllegalAccessException error) { throw new AssertionError(error); }
    }

    static Bitmap decodeStream(InputStream stream, byte[] storage, Rect padding, Options options, long bitmap, long colorSpace) {
        Options decoded = software(options);
        try { return streamImage(stream,storage,padding,decoded,bitmap,colorSpace); }
        finally { outputs(options,decoded); }
    }
    static Bitmap decodeFileDescriptor(FileDescriptor descriptor, Rect padding, Options options, long bitmap, long colorSpace) {
        Options decoded = software(options);
        try { return descriptorImage(descriptor,padding,decoded,bitmap,colorSpace); }
        finally { outputs(options,decoded); }
    }
    static Bitmap decodeAsset(long asset, Rect padding, Options options, long bitmap, long colorSpace) {
        Options decoded = software(options);
        try { return assetImage(asset,padding,decoded,bitmap,colorSpace); }
        finally { outputs(options,decoded); }
    }
    static Bitmap decodeByteArray(byte[] bytes, int offset, int length, Options options, long bitmap, long colorSpace) {
        Options decoded = software(options);
        try { return bytesImage(bytes,offset,length,decoded,bitmap,colorSpace); }
        finally { outputs(options,decoded); }
    }

    private static native Bitmap streamImage(InputStream stream, byte[] storage, Rect padding, Options options, long bitmap, long colorSpace);
    private static native Bitmap descriptorImage(FileDescriptor descriptor, Rect padding, Options options, long bitmap, long colorSpace);
    private static native Bitmap assetImage(long asset, Rect padding, Options options, long bitmap, long colorSpace);
    private static native Bitmap bytesImage(byte[] bytes, int offset, int length, Options options, long bitmap, long colorSpace);
}
