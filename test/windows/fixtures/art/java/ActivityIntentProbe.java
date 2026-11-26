package com.trebledot.stone.probe;

import android.content.ComponentName;
import android.content.Intent;
import java.lang.reflect.Method;

final class ActivityIntentProbe {
    public static final class Game implements java.io.Serializable {
        private static final long serialVersionUID = 1L;
        final String title; final int id;
        Game(String title,int id) { this.title=title; this.id=id; }
    }
    static void local() throws Exception {
        Class<?> codec = Class.forName("com.trebledot.stone.runtime.ArtActivityIntent");
        Method write=codec.getDeclaredMethod("encodeLocal",Intent.class), read=codec.getDeclaredMethod("decodeLocal",String.class,ClassLoader.class);
        write.setAccessible(true); read.setAccessible(true);
        Intent source=new Intent().setComponent(new ComponentName("com.example","Game")).putExtra("game",new Game("Chessmaster",7));
        String bytes=(String)write.invoke(null,source);
        Intent restored=(Intent)read.invoke(null,bytes,ActivityIntentProbe.class.getClassLoader());
        Game game=(Game)restored.getSerializableExtra("game");
        if(game.id!=7 || !game.title.equals("Chessmaster") || !source.filterEquals(restored)) throw new AssertionError("Private process game payload changed");
        android.os.Bundle extra=new android.os.Bundle(); extra.putBinder("binder",new android.os.Binder());
        reject(() -> write.invoke(null,new Intent().putExtras(extra)));
        reject(() -> write.invoke(null,new Intent().addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)));
        reject(() -> write.invoke(null,new Intent().putExtra("oversized",new byte[12289])));
        reject(() -> read.invoke(null,"AAAA",ActivityIntentProbe.class.getClassLoader()));
        System.out.println("PASS same-APK Activity Parcel: application Serializable, class loader, Binder/grant/size/malformed rejection");
    }
    private static Method encode, decode;
    private static String encode(Intent value) throws Exception { return (String)encode.invoke(null,value); }
    private static Intent decode(String value) throws Exception { return (Intent)decode.invoke(null,value); }
    private interface Action { void run() throws Exception; }
    private static void reject(Action action) throws Exception {
        try { action.run(); throw new AssertionError("Unsupported Activity payload accepted"); }
        catch (java.lang.reflect.InvocationTargetException error) {
            if (!(error.getCause() instanceof java.io.IOException) && !(error.getCause() instanceof UnsupportedOperationException)) throw error;
        }
    }
    static void run() throws Exception {
        Class<?> codec = Class.forName("com.trebledot.stone.runtime.ArtActivityIntent");
        encode = codec.getDeclaredMethod("encode",Intent.class); decode = codec.getDeclaredMethod("decode",String.class);
        encode.setAccessible(true); decode.setAccessible(true);
        byte[] array = {0,1,-1};
        Intent source = new Intent("example.action").setDataAndType(android.net.Uri.parse("example://host/中文"),"text/plain")
            .setComponent(new ComponentName("com.example","com.example.Alias")).setPackage("com.example").addCategory("example.category")
            .putExtra("text","中文\n\"\u0000").putExtra("int",-123).putExtra("long",Long.MIN_VALUE).putExtra("bool",true)
            .putExtra("float",1.5f).putExtra("double",-2.5d).putExtra("bytes",array).putExtra("null",(String)null);
        String snapshot = encode(source); array[0] = 42; source.putExtra("text","changed");
        Intent copy = decode(snapshot);
        if (!"中文\n\"\u0000".equals(copy.getStringExtra("text")) || copy.getIntExtra("int",0) != -123
                || copy.getLongExtra("long",0) != Long.MIN_VALUE || !copy.getBooleanExtra("bool",false)
                || copy.getFloatExtra("float",0) != 1.5f || copy.getDoubleExtra("double",0) != -2.5d
                || !java.util.Arrays.equals(copy.getByteArrayExtra("bytes"),new byte[]{0,1,-1})
                || !copy.hasExtra("null") || copy.getStringExtra("null") != null
                || !source.filterEquals(copy)) throw new AssertionError("Typed Activity snapshot changed");
        if (!encode(null).isEmpty() || decode("") != null) throw new AssertionError("Null result Intent changed");
        reject(() -> encode(new Intent().putExtra("object",new ComponentName("com.example","Object"))));
        reject(() -> encode(new Intent().putExtra("nested",new android.os.Bundle())));
        reject(() -> encode(new Intent().addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)));
        reject(() -> encode(new Intent().setIdentifier("unsupported")));
        Intent bounds = new Intent(); bounds.setSourceBounds(new android.graphics.Rect(1,2,3,4)); reject(() -> encode(bounds));
        reject(() -> encode(new Intent().putExtra("bytes",new byte[8193])));
        reject(() -> decode("AAAA"));
        byte[] bytes = android.util.Base64.decode(snapshot,android.util.Base64.NO_WRAP); bytes[3] = 2;
        reject(() -> decode(android.util.Base64.encodeToString(bytes,android.util.Base64.NO_WRAP)));
        byte[] trailing = java.util.Arrays.copyOf(android.util.Base64.decode(snapshot,android.util.Base64.NO_WRAP),bytes.length+1);
        reject(() -> decode(android.util.Base64.encodeToString(trailing,android.util.Base64.NO_WRAP)));
        System.out.println("PASS value-only Activity Intent snapshot: identity, Unicode/null, scalar/byte extras, mutation isolation and unsupported/malformed rejection.");
    }
}
