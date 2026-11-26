package com.trebledot.stone.runtime;

import android.content.pm.ApplicationInfo;
import android.os.Build;
import java.io.*;
import java.util.Arrays;
import java.util.zip.*;

/** Exercises extraction and warm-cache validation on the real image's file APIs. */
public final class NativeLibrariesProbe {
    private static native boolean paintSurface(android.view.Surface surface,int color);
    public static int run(android.content.res.AssetManager assets, String unused) throws Exception {
        verify(new File("/data/native-library-probe"));
        ArtPackage apk = ArtPackage.openApplication(assets,"/data/application.apk");
        ArtApplication app = ArtApplication.start(apk);
        android.app.ActivityManager manager = (android.app.ActivityManager) app.context.getSystemService(android.content.Context.ACTIVITY_SERVICE);
        android.app.ActivityManager.MemoryInfo memory = new android.app.ActivityManager.MemoryInfo();
        manager.getMemoryInfo(memory);
        check(memory.totalMem > 0 && memory.availMem >= 0 && memory.availMem <= memory.totalMem
            && memory.threshold == memory.totalMem/16 && memory.advertisedMem >= memory.totalMem
            && memory.lowMemory == (memory.availMem < memory.threshold), "real ActivityManager memory snapshot");
        System.out.println("PASS image ActivityManager memory query from cached Service snapshot");
        surfaces(app);
        return 42;
    }
    private static void surfaces(ArtApplication app) throws Exception {
        android.app.Activity activity=app.startActivity();
        android.view.SurfaceView surface=new android.view.SurfaceView(activity);
        int[] events={0,0,0};
        surface.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
            public void surfaceCreated(android.view.SurfaceHolder holder) {
                check(holder.getSurface().isValid(),"software Surface validity"); ++events[0];
            }
            public void surfaceChanged(android.view.SurfaceHolder holder,int format,int width,int height) {
                check(format==android.graphics.PixelFormat.RGBA_8888 && width>0 && height>0,"software Surface geometry");
                check(paintSurface(holder.getSurface(),0xff123456),"native Surface lock/post"); ++events[1];
            }
            public void surfaceDestroyed(android.view.SurfaceHolder holder) { ++events[2]; }
        });
        android.widget.FrameLayout layout=new android.widget.FrameLayout(activity);
        layout.addView(surface,new android.widget.FrameLayout.LayoutParams(32,24)); activity.setContentView(layout);
        Object root=ArtWindow.attach(activity);
        Class<?> type=root.getClass();
        java.lang.reflect.Method prepare=type.getDeclaredMethod("prepareFrame",int.class,int.class);
        java.lang.reflect.Method draw=type.getDeclaredMethod("draw",android.graphics.Canvas.class,Class.forName("com.trebledot.stone.runtime.ArtSoftwareRipples"),boolean.class);
        prepare.setAccessible(true); draw.setAccessible(true);
        android.graphics.Bitmap bitmap=android.graphics.Bitmap.createBitmap(360,640,android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas canvas=new android.graphics.Canvas(bitmap);
        try {
            android.view.View decor=activity.getWindow().getDecorView();
            prepare.invoke(root,360,640);
            decor.measure(android.view.View.MeasureSpec.makeMeasureSpec(360,1073741824),android.view.View.MeasureSpec.makeMeasureSpec(640,1073741824));
            decor.layout(0,0,360,640);
            draw.invoke(root,canvas,null,true);
            int[] location=new int[2]; surface.getLocationInWindow(location);
            check(bitmap.getPixel(location[0]+2,location[1]+2)==0xff123456,"native Surface pixels must reach the software frame");
            check(events[0]==1 && events[1]==1 && events[2]==0,"Surface creation callbacks");
            java.lang.reflect.Method destroy=android.view.Surface.class.getDeclaredMethod("destroy"); destroy.setAccessible(true);
            destroy.invoke(surface.getHolder().getSurface());
            check(!surface.getHolder().getSurface().isValid(),"native Surface destroy must invalidate its handle");
            surface.setVisibility(android.view.View.GONE); draw.invoke(root,canvas,null,true);
            check(events[2]==1 && !surface.getHolder().getSurface().isValid(),"Surface visibility cleanup");
            surface.setVisibility(android.view.View.VISIBLE); draw.invoke(root,canvas,null,true);
            check(events[0]==2 && events[1]==2,"Surface recreation callbacks");
        } finally {
            ((AutoCloseable)root).close(); canvas.setBitmap(null); bitmap.recycle(); app.destroyActivity();
        }
        check(events[2]==2 && !surface.getHolder().getSurface().isValid(),"Surface detach cleanup");
        System.out.println("PASS real SurfaceView callbacks, JNI native window conversion, native pixels, hide/show and detach");
    }
    private static void prepare(ApplicationInfo info) throws Exception {
        java.lang.reflect.Method method = Class.forName("com.trebledot.stone.runtime.ArtNativeLibraries")
            .getDeclaredMethod("prepare", ApplicationInfo.class);
        method.setAccessible(true);
        try { method.invoke(null, info); }
        catch (java.lang.reflect.InvocationTargetException error) { throw (Exception) error.getCause(); }
    }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static void archive(File file, String name, byte[] content) throws IOException {
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(file))) {
            zip.putNextEntry(new ZipEntry(name)); zip.write(content); zip.closeEntry();
        }
    }
    public static void verify(File root) throws Exception {
        File fixture = new File(root, "native-libraries");
        check(fixture.isDirectory() || fixture.mkdirs(), "native library fixture directory");
        ApplicationInfo info = new ApplicationInfo();
        info.dataDir = fixture.getPath();
        File source = new File(fixture, "fixture.apk");
        info.sourceDir = source.getPath();
        String prefix = "lib/" + Build.SUPPORTED_64_BIT_ABIS[0] + "/";
        byte[] content = {1, 2, 3, 42};
        archive(source, prefix + "libprobe.so", content);
        prepare(info);
        File library = new File(info.nativeLibraryDir, "libprobe.so");
        check(library.isFile() && !library.canWrite(), "extracted library must be read-only");
        long timestamp = library.lastModified();
        prepare(info);
        check(library.lastModified() == timestamp, "verified warm library must be reused");
        check(library.setWritable(true), "make corrupt cache fixture writable");
        try (FileOutputStream output = new FileOutputStream(library)) { output.write(99); }
        prepare(info);
        try (FileInputStream input = new FileInputStream(library)) {
            byte[] actual = new byte[content.length];
            check(input.read(actual) == actual.length && input.read() == -1 && Arrays.equals(actual, content), "corrupt native cache must be repaired");
        }
        check(!library.canWrite(), "repaired native library must be read-only");
        archive(source, prefix + "../escape.so", content);
        try { prepare(info); throw new AssertionError("native path traversal accepted"); }
        catch (IOException expected) {}
        archive(source, "lib/unsupported-abi/libprobe.so", content);
        try { prepare(info); throw new AssertionError("incompatible APK ABI accepted"); }
        catch (IOException expected) {}
        archive(source, "assets/plain.txt", content);
        prepare(info);
        check(info.nativeLibraryDir == null, "Java-only APK must not bind a native loader");
        System.out.println("PASS APK native libraries: read-only extraction, warm reuse, corruption repair, path and ABI rejection, Java-only APK");
    }
}
