package com.trebledot.stone.probe;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;

/** Public API contract for the ART clipboard, without touching the Windows clipboard. */
final class ClipboardProbe {
    static void verify(Context context) throws Exception {
        ClipboardManager clipboard = context.getSystemService(ClipboardManager.class);
        if (clipboard == null || clipboard.hasPrimaryClip() || clipboard.getPrimaryClip() != null || clipboard.getPrimaryClipDescription() != null)
            throw new AssertionError("Clipboard service or initial empty state");
        int[] changes = {0}; ClipboardManager.OnPrimaryClipChangedListener listener = () -> changes[0]++;
        clipboard.addPrimaryClipChangedListener(listener);
        String text = "Stone 剪贴板\nhttps://example.com/\"quoted\"";
        ClipData input = ClipData.newPlainText("label",text); android.os.PersistableBundle inputExtras = new android.os.PersistableBundle();
        String[] mutable = {"original"}; inputExtras.putStringArray("mutable",mutable); input.getDescription().setExtras(inputExtras); clipboard.setPrimaryClip(input);
        mutable[0] = "modified";
        input.addItem(new ClipData.Item("caller mutation"));
        CommitProbe.await(() -> changes[0] == 1);
        ClipData read = clipboard.getPrimaryClip();
        if (!clipboard.hasPrimaryClip() || !clipboard.hasText() || read == null || read.getItemCount() != 1 || !text.contentEquals(read.getItemAt(0).getText()))
            throw new AssertionError("Clipboard text round trip or input isolation");
        read.addItem(new ClipData.Item("reader mutation"));
        if (!"original".equals(read.getDescription().getExtras().getStringArray("mutable")[0])) throw new AssertionError("Clipboard input metadata was shared");
        read.getDescription().getExtras().getStringArray("mutable")[0] = "reader mutation";
        if (!"original".equals(clipboard.getPrimaryClip().getDescription().getExtras().getStringArray("mutable")[0])) throw new AssertionError("Clipboard output metadata was shared");
        if (clipboard.getPrimaryClip().getItemCount() != 1) throw new AssertionError("Clipboard returned a shared object");
        try { clipboard.setPrimaryClip(read); throw new AssertionError("Unsupported multiple clipboard items were accepted"); }
        catch (UnsupportedOperationException expected) { }
        if (!text.contentEquals(clipboard.getPrimaryClip().getItemAt(0).getText())) throw new AssertionError("Rejected clipboard write changed the old contents");
        clipboard.clearPrimaryClip(); CommitProbe.await(() -> changes[0] == 2);
        if (clipboard.hasPrimaryClip() || clipboard.hasText() || clipboard.getPrimaryClipDescription() != null) throw new AssertionError("Clipboard clear");
        clipboard.removePrimaryClipChangedListener(listener);
        clipboard.setPrimaryClip(ClipData.newPlainText("empty","")); BroadcastProbe.drain();
        if (!clipboard.hasPrimaryClip() || clipboard.hasText() || changes[0] != 2) throw new AssertionError("Empty text or listener removal");
        clipboard.clearPrimaryClip();
        Class<?> adapter = Class.forName("com.trebledot.stone.runtime.ArtClipboard");
        java.lang.reflect.Method encode = adapter.getDeclaredMethod("extras",android.content.ClipDescription.class);
        java.lang.reflect.Method decode = adapter.getDeclaredMethod("from",String[].class); encode.setAccessible(true); decode.setAccessible(true);
        ClipData source = ClipData.newPlainText("transport label",text);
        android.os.PersistableBundle extras = new android.os.PersistableBundle(); extras.putBoolean("android.content.extra.IS_SENSITIVE",true);
        extras.putLong("integer",1234567890123L); extras.putStringArray("values",new String[]{"中文","quoted\""});
        source.getDescription().setExtras(extras); String encoded = (String) encode.invoke(null,source.getDescription());
        ClipData restored = (ClipData) decode.invoke(null,(Object) new String[]{"42","1","1","transport label",text,encoded});
        android.os.PersistableBundle returned = restored.getDescription().getExtras();
        if (!text.contentEquals(restored.getItemAt(0).getText()) || !returned.getBoolean("android.content.extra.IS_SENSITIVE")
                || returned.getLong("integer") != 1234567890123L || !java.util.Arrays.equals(returned.getStringArray("values"),new String[]{"中文","quoted\""}))
            throw new AssertionError("Clipboard description transport lost typed extras");
        restored = (ClipData) decode.invoke(null,(Object) new String[]{"43","1","1","label",text,"bm90IHhtbA=="});
        if (!text.contentEquals(restored.getItemAt(0).getText()) || restored.getDescription().getExtras() != null)
            throw new AssertionError("Malformed external metadata damaged clipboard text");
        String hugeArray = android.util.Base64.encodeToString("<bundle><map><int-array name=\"oversized\" num=\"2147483647\"/></map></bundle>".getBytes("UTF-8"),android.util.Base64.NO_WRAP);
        restored = (ClipData) decode.invoke(null,(Object) new String[]{"44","1","1","label",text,hugeArray});
        if (restored.getDescription().getExtras() != null) throw new AssertionError("Oversized external array metadata was accepted");
        android.os.IBinder binder = (android.os.IBinder) Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"clipboard");
        Object service = binder.queryLocalInterface("android.content.IClipboard");
        java.lang.reflect.Method has = Class.forName("android.content.IClipboard").getMethod("hasPrimaryClip",String.class,String.class,int.class,int.class);
        int user = android.os.Process.myUid() / 100000;
        for (Object[] caller : new Object[][]{{"com.other.app",null,user,0},{context.getPackageName(),null,user+1,0},{context.getPackageName(),null,user,1}}) {
            try { has.invoke(service,caller); throw new AssertionError("Clipboard accepted another caller, user or device"); }
            catch (java.lang.reflect.InvocationTargetException error) { if (!(error.getCause() instanceof SecurityException)) throw error; }
        }
        System.out.println("ART ClipboardManager text, object isolation, listeners and clear passed without Windows clipboard access.");
    }
}
