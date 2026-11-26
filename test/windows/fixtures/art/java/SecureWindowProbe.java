package com.trebledot.stone.probe;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.view.View;
import android.view.WindowManager;
import java.io.File;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

final class SecureWindowProbe {
    static boolean secure(Activity activity) throws Exception {
        java.lang.reflect.Field field = Class.forName("com.trebledot.stone.runtime.ArtServices").getDeclaredField("windows");
        field.setAccessible(true); Object windows = field.get(null);
        Method policy = windows.getClass().getDeclaredMethod("captureSecure",Activity.class); policy.setAccessible(true);
        return (Boolean) policy.invoke(windows,activity);
    }
    static int nativeMode() throws Exception {
        Method mode = com.trebledot.stone.runtime.ArtWindow.class.getDeclaredMethod("captureProtection"); mode.setAccessible(true);
        return (Integer) mode.invoke(null);
    }
    static void requirePolicy(boolean expected) {
        try { if (secure(null) != expected) throw new AssertionError("Active window capture policy differs: " + expected); }
        catch (Exception error) { throw new AssertionError("Cannot read active capture policy",error); }
    }
    static void writePng(File file, Bitmap bitmap, boolean secure) throws Exception {
        Method write = Class.forName("com.trebledot.stone.runtime.ArtDiagnostics").getDeclaredMethod("writeFrame",File.class,Bitmap.class,boolean.class);
        write.setAccessible(true); write.invoke(null,file,bitmap,secure);
    }
    static void verifyPolicy(Activity activity) throws Exception {
        int original = activity.getWindow().getAttributes().flags;
        if ((original & WindowManager.LayoutParams.FLAG_SECURE) != 0) throw new AssertionError("CIT capture fixture unexpectedly secure");
        Dialog dialog = new Dialog(activity);
        try {
            if (secure(activity)) throw new AssertionError("Public Activity was marked secure");
            activity.getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
            if (!secure(activity)) throw new AssertionError("Activity FLAG_SECURE was ignored");
            activity.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_SECURE);
            android.widget.TextView text = new android.widget.TextView(activity); text.setText("Secure child fixture");
            dialog.setContentView(text); dialog.getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE); dialog.show();
            View decor = dialog.getWindow().getDecorView();
            if (!secure(activity)) throw new AssertionError("Visible secure dialog did not protect its host");
            decor.setVisibility(View.GONE);
            if (secure(activity)) throw new AssertionError("Hidden secure dialog protected an unrelated public frame");
            decor.setVisibility(View.VISIBLE);
            if (!secure(activity)) throw new AssertionError("Restored secure dialog lost capture protection");
            dialog.dismiss();
            if (secure(activity)) throw new AssertionError("Removed secure dialog retained capture protection");
            System.out.println("PASS secure Activity and visible child aggregation; hide, restore and removal.");
        } finally { dialog.dismiss(); activity.getWindow().setFlags(original,WindowManager.LayoutParams.FLAG_SECURE); }
    }
    static boolean diagnosticReady(Activity activity, boolean secure) throws Exception {
        File directory = new File("/data/stone-diagnostics",activity.getPackageName());
        File state = new File(directory,"state.txt"), file = new File(directory,"frame.png");
        if (!state.isFile() || !file.isFile()) return false;
        String metadata = new String(Files.readAllBytes(state.toPath()),StandardCharsets.UTF_8);
        if (secure ? !metadata.equals("capture=redacted\nreason=FLAG_SECURE\n") : !metadata.startsWith("activity=")) return false;
        Bitmap bitmap = BitmapFactory.decodeFile(file.getPath()); if (bitmap == null) return false;
        try {
            int[] pixels = new int[bitmap.getWidth()*bitmap.getHeight()]; bitmap.getPixels(pixels,0,bitmap.getWidth(),0,0,bitmap.getWidth(),bitmap.getHeight());
            boolean black = pixels.length > 0;
            for (int pixel : pixels) if (pixel != android.graphics.Color.BLACK) { black = false; break; }
            return black == secure;
        } finally { bitmap.recycle(); }
    }
}
