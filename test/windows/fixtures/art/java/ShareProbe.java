package com.trebledot.stone.probe;

import android.content.ClipData;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import java.lang.reflect.Method;

/** Android share parsing without opening the system UI or selecting a target. */
final class ShareProbe {
    static void verify(Context context) throws Exception {
        PackageManager packages = context.getPackageManager();
        Method content = Class.forName("com.trebledot.stone.runtime.ArtShares").getDeclaredMethod("content",Intent.class); content.setAccessible(true);
        String body = "分享 🚀\n\"quoted\" https://example.com/";
        android.text.SpannableStringBuilder mutable = new android.text.SpannableStringBuilder(body);
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT,mutable).putExtra(Intent.EXTRA_SUBJECT,"Stone sample");
        Intent chooser = Intent.createChooser(send,"Choose an application");
        for (Intent intent : new Intent[]{send,chooser}) {
            ComponentName component = intent.resolveActivity(packages);
            if (component == null || !"com.trebledot.stone.share".equals(component.getPackageName()) || !packages.getActivityInfo(component,0).exported)
                throw new AssertionError("Windows share component missing");
            String[] snapshot = (String[]) content.invoke(null,intent);
            if (!"Stone sample".equals(snapshot[0]) || !body.equals(snapshot[1])) throw new AssertionError("Share title/body changed");
        }
        String[] snapshot = (String[]) content.invoke(null,send); mutable.append("changed");
        if (!body.equals(snapshot[1])) throw new AssertionError("Mutable Android share object escaped");
        Intent text = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT,"plain");
        for (Intent invalid : new Intent[]{new Intent(text).putExtra(Intent.EXTRA_SUBJECT,12),new Intent(text).putExtra(Intent.EXTRA_TITLE,true),
                Intent.createChooser(text,null).putExtra(Intent.EXTRA_TITLE,12)})
            if (invalid.resolveActivity(packages) != null) throw new AssertionError("Incorrectly typed share caption was silently discarded");
        String limit = new String(new char[8192]).replace('\0','a');
        if (new Intent(text).putExtra(Intent.EXTRA_TEXT,limit).resolveActivity(packages) == null) throw new AssertionError("Valid share text limit rejected");
        for (String invalid : new String[]{"a\0b",limit + "a"})
            if (new Intent(text).putExtra(Intent.EXTRA_TEXT,invalid).resolveActivity(packages) != null) throw new AssertionError("Invalid share text length/NUL accepted");
        text.setClipData(ClipData.newPlainText("label","plain"));
        if (text.resolveActivity(packages) == null || Intent.createChooser(text,null).resolveActivity(packages) == null)
            throw new AssertionError("Framework text ClipData migration rejected");
        Intent foreignClip = new Intent(text); foreignClip.setClipData(new ClipData("file",new String[]{"text/plain"},new ClipData.Item(Uri.parse("content://private/file"))));
        Intent wrongChooser = Intent.createChooser(text,null).putExtra(Intent.EXTRA_INITIAL_INTENTS,new Intent[]{text});
        for (Intent invalid : new Intent[]{new Intent(text).setAction(Intent.ACTION_SEND_MULTIPLE),new Intent(text).setType("text/html"),
                new Intent(text).putExtra(Intent.EXTRA_STREAM,Uri.parse("content://private/file")),foreignClip,wrongChooser,
                new Intent(text).putExtra(Intent.EXTRA_EMAIL,new String[]{"unknown@example.com"}),new Intent(text).setPackage("com.other.app"),
                new Intent(text).putExtra(Intent.EXTRA_TEXT,"a\0b"),new Intent(text).putExtra(Intent.EXTRA_TEXT,new String(new char[8193]).replace('\0','a'))})
            if (invalid.resolveActivity(packages) != null) throw new AssertionError("Unsupported share payload was silently discarded: " + invalid);
        Uri file = Uri.parse("content://com.trebledot.stone.files/example.apk");
        Intent apk = new Intent(Intent.ACTION_SEND).setDataAndType(file,"application/vnd.android.package-archive")
            .putExtra(Intent.EXTRA_STREAM,file).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        apk.setClipData(new ClipData("apk",new String[]{"application/vnd.android.package-archive"},new ClipData.Item(file)));
        if (apk.resolveActivity(packages) == null || Intent.createChooser(apk,null).resolveActivity(packages) == null)
            throw new AssertionError("Read-granted APK share did not resolve");
        for (Intent invalid : new Intent[]{new Intent(apk).setFlags(0),new Intent(apk).addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION),
                new Intent(apk).addFlags(Intent.FLAG_GRANT_PREFIX_URI_PERMISSION),new Intent(apk).addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION),
                new Intent(apk).setDataAndType(Uri.parse("content://other/file"),"application/vnd.android.package-archive"),new Intent(apk).putExtra(Intent.EXTRA_TEXT,"discarded"),
                new Intent(apk).putExtra(Intent.EXTRA_STREAM,Uri.parse("file:///data/application.apk")),
                new Intent(apk).setAction(Intent.ACTION_SEND_MULTIPLE),new Intent(apk).putExtra(Intent.EXTRA_SUBJECT,12)})
            if (invalid.resolveActivity(packages) != null) throw new AssertionError("Unsupported APK share resolved: " + invalid);
        Method filename = Class.forName("com.trebledot.stone.runtime.ArtShareFiles").getDeclaredMethod("filename",String.class); filename.setAccessible(true);
        for (String invalid : new String[]{"../app.apk","C:\\private.apk","app:stream.apk",".hidden.apk","app.exe","中文.apk","CON.apk","com1.apk"})
            if (!"shared.apk".equals(filename.invoke(null,invalid))) throw new AssertionError("Unsafe Windows share filename accepted");
        System.out.println("Android share SEND/CHOOSER, mutable text isolation, migrated ClipData and unsupported payload rejection passed; no share UI opened.");
    }
}
