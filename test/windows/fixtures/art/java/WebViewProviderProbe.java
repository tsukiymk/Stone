package com.trebledot.stone.runtime;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.os.IBinder;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.StringReader;
import java.lang.reflect.InvocationTargetException;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** Exercises discovery and explicit unavailability on the selected image's ART and verifier. */
public final class WebViewProviderProbe {
    private static void check(boolean value,String message) { if(!value) throw new AssertionError(message); }
    private static boolean networkGranted(PackageInfo info,String name) throws Exception {
        java.lang.reflect.Method method=Class.forName("com.trebledot.stone.runtime.ArtPermissions")
            .getDeclaredMethod("networkGranted",PackageInfo.class,String.class);
        method.setAccessible(true); return (Boolean)method.invoke(null,info,name);
    }
    private interface Checked { void run() throws Exception; }
    private static Object invoke(String name,Class<?>[] types,Object... arguments) throws Exception {
        java.lang.reflect.Method method=Class.forName("com.trebledot.stone.runtime.ArtWebView").getDeclaredMethod(name,types);
        method.setAccessible(true);
        try { return method.invoke(null,arguments); }
        catch(InvocationTargetException error) {
            if(error.getCause() instanceof Exception) throw (Exception)error.getCause();
            throw (Error)error.getCause();
        }
    }
    private static Object field(Object object,String name) throws Exception {
        java.lang.reflect.Field field=object.getClass().getDeclaredField(name); field.setAccessible(true);
        return field.get(object);
    }
    @SuppressWarnings("unchecked")
    private static List<String> index(File file) throws Exception {
        return (List<String>)invoke("readIndex",new Class<?>[]{File.class},file);
    }
    private static Object inspect(PackageManager manager,String path) throws Exception {
        return invoke("inspect",new Class<?>[]{PackageManager.class,String.class},manager,path);
    }
    private static String engineProblem(PackageInfo info) throws Exception {
        return (String)invoke("engineProblem",new Class<?>[]{PackageInfo.class},info);
    }
    private static Object provider(String name,String certificate) throws Exception {
        java.lang.reflect.Constructor<?> constructor=Class.forName("com.trebledot.stone.runtime.ArtWebView$Provider")
            .getDeclaredConstructor(String.class,String.class,boolean.class,boolean.class,String[].class);
        constructor.setAccessible(true);
        return constructor.newInstance(name,"test",true,false,new String[]{certificate});
    }
    private static boolean matches(Object provider,PackageInfo info) throws Exception {
        return (Boolean)invoke("matches",new Class<?>[]{provider.getClass(),PackageInfo.class},provider,info);
    }
    private static void rejects(Checked action) throws Exception {
        try { action.run(); } catch(IOException expected) { return; }
        throw new AssertionError("Malformed provider input was accepted");
    }
    private static List<?> configuration(String xml) throws Exception {
        org.xmlpull.v1.XmlPullParser parser=android.util.Xml.newPullParser();
        parser.setInput(new StringReader(xml));
        return (List<?>)invoke("readProviders",new Class<?>[]{org.xmlpull.v1.XmlPullParser.class},parser);
    }
    private static void write(File file,String contents) throws IOException {
        try(FileOutputStream output=new FileOutputStream(file)) { output.write(contents.getBytes(StandardCharsets.UTF_8)); }
    }
    private static void inputs() throws Exception {
        String provider="<webviewprovider packageName='com.example.webview' description='Provider' availableByDefault='true'/>";
        List<?> rules=configuration("<webviewproviders>"+provider+"</webviewproviders>");
        check(rules.size()==1 && (Boolean)field(rules.get(0),"available") && !(Boolean)field(rules.get(0),"fallback"),"image configuration attributes");
        rejects(() -> configuration("<wrong/>"));
        rejects(() -> configuration("<webviewproviders>"+provider+provider+"</webviewproviders>"));
        rejects(() -> configuration("<webviewproviders>"+provider.replace("'true'","'yes'")+"</webviewproviders>"));
        rejects(() -> configuration("<webviewproviders><webviewprovider packageName='com.example.webview' description='Provider' isFallback='true'/></webviewproviders>"));
        rejects(() -> configuration("<webviewproviders><webviewprovider packageName='com.example.webview' description='Provider'><signature>!</signature></webviewprovider></webviewproviders>"));
        File index=new File("/data/webview-index-test.txt");
        check(index(new File("/data/no-webview-index-test.txt")).isEmpty(),"old caches have no candidates");
        String candidate="/product/app/WebViewGoogle-Stub/WebViewGoogle-Stub.apk";
        write(index,candidate+"\n");
        check(index(index).equals(java.util.Collections.singletonList(candidate)),"read-only candidate path");
        for(String bad:new String[]{"/data/app/provider.apk", "/product/app/../WebView/provider.apk", "/product/app/WebView/base.apk:stream",
                "/product/app/WebView\\escape/base.apk", candidate+"\n"+candidate, ""}) {
            write(index,bad+"\n"); rejects(() -> index(index));
        }
        write(index,""); check(index(index).isEmpty(),"empty image inventory");
        write(index,String.join("",java.util.Collections.nCopies(65537,"x"))); rejects(() -> index(index));
        check(index.delete(),"index fixture cleanup");
        System.out.println("PASS WebView configuration parsing, read-only path boundaries, duplicates and index limits");
    }
    private static void signatures(PackageManager manager) throws Exception {
        String path="/data/webview-fixtures/v2.apk";
        Object inspected=inspect(manager,path);
        check(field(inspected,"problem")!=null && ((String)field(inspected,"problem")).contains("metadata"),"signed non-provider must not become an engine");
        PackageInfo info=(PackageInfo)field(inspected,"info");
        String certificate=android.util.Base64.encodeToString(info.signingInfo.getApkContentsSigners()[0].toByteArray(),android.util.Base64.NO_WRAP);
        Object matching=provider(info.packageName,certificate);
        check(matches(matching,info),"configured provider signer");
        check(!matches(provider("com.example.other",certificate),info),"provider package identity");
        check(!matches(provider(info.packageName,"AQID"),info),"provider signer mismatch");
        rejects(() -> inspect(manager,"/data/webview-fixtures/tampered-v2.apk"));
        rejects(() -> inspect(manager,"/data/webview-fixtures/unsigned.apk"));
        info.applicationInfo.metaData=new android.os.Bundle();
        info.applicationInfo.metaData.putString("com.android.webview.WebViewLibrary","../libunsafe.so");
        check(engineProblem(info).contains("invalid"),"native library metadata cannot contain a path");
        info.applicationInfo.metaData.putString("com.android.webview.WebViewLibrary","libwebview_fixture.so");
        info.applicationInfo.enabled=false;
        check(engineProblem(info).contains("disabled"),"disabled provider rejection");
        info.applicationInfo.enabled=true;
        info.applicationInfo.flags&=~android.content.pm.ApplicationInfo.FLAG_HAS_CODE;
        check(engineProblem(info).contains("application code"),"manifest without code rejection");
        info.applicationInfo.flags|=android.content.pm.ApplicationInfo.FLAG_HAS_CODE;
        check(engineProblem(info).contains("DEX"),"metadata alone does not provide executable DEX");
        PackageInfo code=(PackageInfo)field(inspect(manager,"/data/webview-fixtures/launcher.apk"),"info");
        code.applicationInfo.metaData=info.applicationInfo.metaData;
        check(engineProblem(code).contains("native library"),"DEX alone does not provide the active-ABI engine");
        System.out.println("PASS image full-content APK verification, configured signer matching and unsigned/tampered rejection");
    }
    private static void service(boolean selected) throws Exception {
        Class<?> manager=Class.forName("android.os.ServiceManager"), type=Class.forName("android.webkit.IWebViewUpdateService");
        IBinder binder=(IBinder)manager.getMethod("getService",String.class).invoke(null,"webviewupdate");
        check(binder!=null,"WebView update service is registered");
        Object service=binder.queryLocalInterface(type.getName());
        Object[] all=(Object[])type.getMethod("getAllWebViewPackages").invoke(service);
        check(all!=null && all.length>0,"image provider allowlist");
        String name=(String)all[0].getClass().getField("packageName").get(all[0]);
        all[0]=null;
        Object[] again=(Object[])type.getMethod("getAllWebViewPackages").invoke(service);
        check(name.equals(again[0].getClass().getField("packageName").get(again[0])),"provider query results are independent");
        check((type.getMethod("getCurrentWebViewPackage").invoke(service)!=null)==selected
            && (type.getMethod("getCurrentWebViewPackageName").invoke(service)!=null)==selected,"current engine selection");
        Object response=type.getMethod("waitForAndGetProvider").invoke(service);
        int status=response.getClass().getField("status").getInt(response);
        check(selected?status==3 && response.getClass().getField("packageInfo").get(response)!=null:
            (status==4 || status==11) && response.getClass().getField("packageInfo").get(response)==null,
            "unconnected engine must not report success or RELRO-only failure");
        try {
            type.getMethod("changeProviderAndSetting",String.class).invoke(service,"com.example.webview");
            throw new AssertionError("Application changed provider settings");
        } catch(InvocationTargetException denied) { check(denied.getCause() instanceof SecurityException,"provider settings permission"); }
        System.out.println("PASS real image WebView update Binder queries, selected="+selected+", provider status="+status);
    }
    public static int run(AssetManager assets,String unused) throws Exception {
        inputs();
        ArtPackage apk=ArtPackage.openApplication(assets,"/data/application.apk");
        ArtApplication app=ArtApplication.start(apk);
        for(String permission:new String[]{"android.permission.INTERNET","android.permission.ACCESS_NETWORK_STATE"}) {
            check(!networkGranted(new PackageInfo(),permission),"undeclared network permission");
            check(app.context.checkSelfPermission(permission)==PackageManager.PERMISSION_GRANTED,"declared network permission");
            check(app.context.getPackageManager().checkPermission(permission,app.context.getPackageName())==PackageManager.PERMISSION_GRANTED,"package network permission query");
            check(app.context.checkPermission(permission,android.os.Process.myPid(),android.os.Process.myUid()+1)==PackageManager.PERMISSION_DENIED,"foreign UID network permission");
            check(app.context.getPackageManager().checkPermission(permission,"com.example.foreign")==PackageManager.PERMISSION_DENIED,"foreign package network permission");
        }
        PackageInfo permissionInfo=app.context.getPackageManager().getPackageInfo(app.context.getPackageName(),PackageManager.GET_PERMISSIONS);
        for(int index=0;index<permissionInfo.requestedPermissions.length;index++)
            if(networkGranted(permissionInfo,permissionInfo.requestedPermissions[index]))
                check((permissionInfo.requestedPermissionsFlags[index]&PackageInfo.REQUESTED_PERMISSION_GRANTED)!=0,"network permission grant metadata");
        signatures(app.context.getPackageManager());
        List<String> candidates=index(new File("/system/etc/stone-webview-apks.list"));
        System.out.println("Image WebView candidate count="+candidates.size());
        for(String path:candidates) {
            Object candidate=inspect(app.context.getPackageManager(),path);
            PackageInfo info=(PackageInfo)field(candidate,"info");
            System.out.println("Verified image WebView candidate "+info.packageName+" "+info.versionName+": "+field(candidate,"problem"));
        }
        boolean selected=app.context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WEBVIEW);
        service(selected);
        if(selected) return WebViewRenderProbe.run(app);
        check(!app.context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_WEBVIEW)
            && android.webkit.WebView.getCurrentWebViewPackage()==null,"public WebView capability remains unavailable");
        System.out.println("PASS WebView discovery without advertising a usable provider or loading a Windows JVM");
        return 42;
    }
}
