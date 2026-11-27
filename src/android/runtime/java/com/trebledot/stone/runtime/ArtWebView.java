package com.trebledot.stone.runtime;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.os.Build;
import android.util.Log;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.lang.reflect.Array;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/** Discovers and loads a provider from the selected read-only image. */
final class ArtWebView {
    private static final String TAG = "StoneWebView";
    private static final File INDEX = new File("/system/etc/stone-webview-apks.list");
    private final List<Provider> providers;
    private List<Candidate> candidates;
    private PackageInfo selected;
    private PackageInfo pending;
    private android.content.Context providerContext;
    private final Set<String> dependencies=java.util.Collections.synchronizedSet(new HashSet<>());
    private static native void bindNativeLibraries(ClassLoader loader,String source,String directory,String data) throws IOException;
    private static native void loadNativeLibrary(Class<?> owner,String path);

    boolean supported() { return selected!=null; }
    String packageName() { return selected==null?null:selected.packageName; }

    void initialize(android.app.Application application,ArtPackage running) {
        try {
            for(Candidate candidate:discover()) {
                if(candidate.problem!=null || providers.stream().noneMatch(provider -> provider.available && matches(provider,candidate.info))) continue;
                PackageInfo info=candidate.info;
                info.applicationInfo.uid=ArtServices.packages.reserveImageUid(info.packageName);
                info.applicationInfo.flags|=android.content.pm.ApplicationInfo.FLAG_SYSTEM|android.content.pm.ApplicationInfo.FLAG_INSTALLED;
                info.applicationInfo.dataDir="/data/data/"+info.packageName;
                info.applicationInfo.deviceProtectedDataDir=info.applicationInfo.dataDir+"/device-protected";
                String library=info.applicationInfo.metaData.getString("com.android.webview.WebViewLibrary");
                info.applicationInfo.nativeLibraryDir=ArtNativeLibraries.prepareImageProvider(info.applicationInfo,running.applicationInfo.dataDir,library);
                android.content.pm.ApplicationInfo.class.getField("primaryCpuAbi").set(info.applicationInfo,Build.SUPPORTED_64_BIT_ABIS[0]);
                for(android.content.pm.ComponentInfo[] group:new android.content.pm.ComponentInfo[][]{info.activities,info.receivers,info.services,info.providers})
                    if(group!=null) for(android.content.pm.ComponentInfo component:group) component.applicationInfo=info.applicationInfo;
                pending=info;
                ClassLoader loader=new dalvik.system.PathClassLoader(info.applicationInfo.sourceDir,
                    info.applicationInfo.nativeLibraryDir,ArtBoot.class.getClassLoader());
                providerContext=ArtApplication.imageCodeContext(application,info.applicationInfo,loader);
                bindNativeLibraries(loader,info.applicationInfo.sourceDir,info.applicationInfo.nativeLibraryDir,running.applicationInfo.dataDir);
                Class<?> factory=Class.forName("com.android.webview.chromium.WebViewChromiumFactoryProviderForT",false,loader);
                loadNativeLibrary(factory,new File(info.applicationInfo.nativeLibraryDir,library).getPath());
                selected=info;
                Log.i(TAG,"Loaded image WebView native library: "+info.packageName+" "+info.versionName);
                return;
            }
        } catch(Exception | LinkageError error) { Log.e(TAG,"Could not load image WebView provider",error); }
        finally { pending=null; }
    }

    boolean noteDependency(String name) {
        if((selected!=null && selected.packageName.equals(name)) || (pending!=null && pending.packageName.equals(name))) {
            dependencies.add(name); return true;
        }
        return false;
    }

    PackageInfo packageInfo(String name,long flags) {
        PackageInfo available=selected==null?pending:selected;
        if(available==null || !available.packageName.equals(name)) return null;
        android.os.Parcel parcel=android.os.Parcel.obtain();
        try {
            available.writeToParcel(parcel,0); parcel.setDataPosition(0);
            PackageInfo result=PackageInfo.CREATOR.createFromParcel(parcel);
            if((flags & PackageManager.GET_SIGNATURES)==0) result.signatures=null;
            if((flags & PackageManager.GET_SIGNING_CERTIFICATES)==0) result.signingInfo=null;
            if((flags & PackageManager.GET_META_DATA)==0) result.applicationInfo.metaData=null;
            if((flags & PackageManager.GET_ACTIVITIES)==0) result.activities=null;
            if((flags & PackageManager.GET_RECEIVERS)==0) result.receivers=null;
            if((flags & PackageManager.GET_SERVICES)==0) result.services=null;
            if((flags & PackageManager.GET_PROVIDERS)==0) result.providers=null;
            if((flags & PackageManager.GET_PERMISSIONS)==0) { result.permissions=null;result.requestedPermissions=null;result.requestedPermissionsFlags=null; }
            for(android.content.pm.ComponentInfo[] group:new android.content.pm.ComponentInfo[][]{result.activities,result.receivers,result.services,result.providers})
                if(group!=null) for(android.content.pm.ComponentInfo component:group) {
                    component.applicationInfo=result.applicationInfo;
                    if((flags & PackageManager.GET_META_DATA)==0) component.metaData=null;
                }
            return result;
        } finally { parcel.recycle(); }
    }

    static final class Provider {
        final String name, description;
        final boolean available, fallback;
        final String[] certificates;
        Provider(String name, String description, boolean available, boolean fallback, String[] certificates) {
            this.name=name; this.description=description; this.available=available; this.fallback=fallback;
            this.certificates=certificates.clone();
        }
        Object info() throws ReflectiveOperationException {
            return Class.forName("android.webkit.WebViewProviderInfo")
                .getConstructor(String.class,String.class,boolean.class,boolean.class,String[].class)
                .newInstance(name,description,available,fallback,certificates.clone());
        }
    }

    static final class Candidate {
        final PackageInfo info;
        final String problem;
        Candidate(PackageInfo info,String problem) { this.info=info; this.problem=problem; }
    }

    ArtWebView() {
        List<Provider> configuration;
        Resources system=Resources.getSystem();
        int id=system.getIdentifier("config_webview_packages","xml","android");
        try {
            if(id==0) throw new IOException("Image has no WebView provider configuration");
            try(XmlResourceParser parser=system.getXml(id)) { configuration=readProviders(parser); }
        } catch(IOException | XmlPullParserException | RuntimeException error) {
            Log.w(TAG,"Cannot read image WebView provider configuration",error);
            configuration=new ArrayList<>();
        }
        providers=configuration;
    }

    static List<Provider> readProviders(XmlPullParser parser) throws IOException,XmlPullParserException {
        List<Provider> result=new ArrayList<>();
        Set<String> names=new HashSet<>();
        int event;
        do { event=parser.next(); } while(event!=XmlPullParser.START_TAG && event!=XmlPullParser.END_DOCUMENT);
        if(event!=XmlPullParser.START_TAG || !"webviewproviders".equals(parser.getName()))
            throw new IOException("Invalid WebView configuration root");
        int root=parser.getDepth();
        while((event=parser.next())!=XmlPullParser.END_DOCUMENT) {
            if(event==XmlPullParser.END_TAG && parser.getDepth()==root) break;
            if(event!=XmlPullParser.START_TAG) continue;
            if(parser.getDepth()!=root+1 || !"webviewprovider".equals(parser.getName()))
                throw new IOException("Unexpected WebView configuration element");
            String name=parser.getAttributeValue(null,"packageName");
            ArtPackageStore.validateName(name);
            if(result.size()>=32 || !names.add(name)) throw new IOException("Duplicate or excessive WebView providers");
            String description=parser.getAttributeValue(null,"description");
            if(description==null) throw new IOException("Missing WebView provider description");
            boolean available=booleanAttribute(parser,"availableByDefault"), fallback=booleanAttribute(parser,"isFallback");
            if(fallback && !available) throw new IOException("Fallback WebView provider must be available by default");
            List<String> certificates=new ArrayList<>();
            int depth=parser.getDepth();
            while((event=parser.next())!=XmlPullParser.END_DOCUMENT) {
                if(event==XmlPullParser.END_TAG && parser.getDepth()==depth) break;
                if(event!=XmlPullParser.START_TAG) continue;
                if(parser.getDepth()!=depth+1 || !"signature".equals(parser.getName()) || certificates.size()>=16)
                    throw new IOException("Invalid WebView signature configuration");
                String certificate=parser.nextText().trim();
                try {
                    byte[] bytes=android.util.Base64.decode(certificate,android.util.Base64.DEFAULT);
                    if(bytes.length==0 || bytes.length>16384) throw new IllegalArgumentException("Invalid certificate length");
                } catch(IllegalArgumentException error) { throw new IOException("Invalid WebView certificate",error); }
                certificates.add(certificate);
            }
            result.add(new Provider(name,description,available,fallback,certificates.toArray(new String[0])));
        }
        return result;
    }

    private static boolean booleanAttribute(XmlPullParser parser,String name) throws IOException {
        String value=parser.getAttributeValue(null,name);
        if(value==null || "false".equals(value)) return false;
        if("true".equals(value)) return true;
        throw new IOException("Invalid WebView boolean: "+name);
    }

    static List<String> readIndex(File file) throws IOException {
        List<String> result=new ArrayList<>();
        if(!file.exists()) return result; // Caches prepared before provider discovery.
        if(!file.isFile() || file.length()>65536) throw new IOException("Invalid WebView APK index size");
        Set<String> unique=new HashSet<>();
        try(BufferedReader input=new BufferedReader(new InputStreamReader(new FileInputStream(file),StandardCharsets.UTF_8))) {
            for(String path; (path=input.readLine())!=null;) {
                if(result.size()>=128 || !path.matches("/(system|product|system_ext)/(app|priv-app)/[^/]+/[^/]+\\.apk")
                        || path.indexOf('\\')>=0 || path.indexOf(':')>=0 || !unique.add(path)
                        || !new File(path).getCanonicalPath().equals(path))
                    throw new IOException("Invalid WebView APK index path: "+path);
                result.add(path);
            }
        }
        return result;
    }

    static Candidate inspect(PackageManager manager,String path) throws IOException {
        PackageInfo info=ArtArchive.inspect(manager,path); // Full-content verification, including v1/v2/v3 policy.
        return new Candidate(info,engineProblem(info));
    }

    static String engineProblem(PackageInfo info) throws IOException {
        String library=info.applicationInfo.metaData==null?null:
            info.applicationInfo.metaData.getString("com.android.webview.WebViewLibrary");
        if(library==null) return "missing WebViewLibrary metadata (stub or non-provider APK)";
        if(!library.matches("lib[A-Za-z0-9_+.-]+\\.so")) return "invalid WebView native library name";
        if(!info.applicationInfo.enabled) return "provider is disabled";
        if((info.applicationInfo.flags & android.content.pm.ApplicationInfo.FLAG_HAS_CODE)==0)
            return "provider has no application code";
        String abi=ArtPackageCompatibility.imageAbi(Build.SUPPORTED_64_BIT_ABIS);
        try(ZipFile zip=new ZipFile(info.applicationInfo.sourceDir)) {
            ZipEntry dex=zip.getEntry("classes.dex"), nativeCode=zip.getEntry("lib/"+abi+"/"+library);
            if(dex==null || dex.isDirectory() || dex.getSize()<112)
                return "provider has no DEX code";
            if((nativeCode==null || nativeCode.isDirectory() || nativeCode.getSize()<64) && ArtNativeLibraries.imageLibrary(info.applicationInfo,library)==null)
                return "provider native library for "+abi+" is absent; shared-library resolution is not connected";
        }
        return null;
    }

    static boolean matches(Provider provider,PackageInfo info) {
        if(!provider.name.equals(info.packageName)) return false;
        if(provider.certificates.length==0) return true; // Only immutable APKs from the selected image are considered.
        Signature[] actual=info.signingInfo.getApkContentsSigners();
        if(actual.length!=1) return false;
        for(String certificate:provider.certificates)
            if(actual[0].equals(new Signature(android.util.Base64.decode(certificate,android.util.Base64.DEFAULT)))) return true;
        return false;
    }

    private synchronized List<Candidate> discover() throws ReflectiveOperationException {
        if(candidates!=null) return candidates;
        List<Candidate> found=new ArrayList<>();
        android.app.Application application=(android.app.Application)Class.forName("android.app.AppGlobals")
            .getMethod("getInitialApplication").invoke(null);
        if(application==null) throw new IllegalStateException("WebView discovery requires an application context");
        try {
            for(String path:readIndex(INDEX)) {
                try {
                    Candidate inspected=inspect(application.getPackageManager(),path);
                    Candidate candidate=inspected.problem==null && providers.stream().noneMatch(provider -> matches(provider,inspected.info))
                        ?new Candidate(inspected.info,"package or certificate does not match image WebView configuration"):inspected;
                    Log.i(TAG,candidate.info.packageName+" "+candidate.info.versionName+": "+
                        (candidate.problem==null?"APK resources and native library verified":candidate.problem));
                    found.add(candidate);
                } catch(IOException | RuntimeException error) { Log.w(TAG,"Rejected WebView candidate "+path,error); }
            }
        } catch(IOException error) { Log.w(TAG,"Cannot read prepared WebView APK index",error); }
        candidates=found;
        return found;
    }

    Object call(Object instance,Method method,Object[] arguments) throws ReflectiveOperationException {
        switch(method.getName()) {
            case "getAllWebViewPackages": {
                Class<?> type=Class.forName("android.webkit.WebViewProviderInfo");
                Object result=Array.newInstance(type,providers.size());
                for(int i=0;i<providers.size();i++) Array.set(result,i,providers.get(i).info());
                return result;
            }
            case "getDefaultWebViewPackage":
                for(Provider provider:providers) if(provider.available) return provider.info();
                throw new UnsupportedOperationException("Image has no default WebView provider configuration");
            case "getCurrentWebViewPackage": discover(); return selected==null?null:packageInfo(selected.packageName,~0L);
            case "getCurrentWebViewPackageName": discover(); return selected==null?null:selected.packageName;
            case "waitForAndGetProvider": {
                boolean engine=discover().stream().anyMatch(candidate -> candidate.problem==null);
                // Missing shared RELRO is valid only after the original native library loaded.
                // Otherwise report listing failure or native-load failure, without a selected package.
                return Class.forName("android.webkit.WebViewProviderResponse").getConstructor(PackageInfo.class,int.class)
                    .newInstance(selected==null?null:packageInfo(selected.packageName,~0L),selected!=null?3:engine?11:4);
            }
            case "changeProviderAndSetting":
            case "notifyRelroCreationCompleted":
            case "getValidWebViewPackages":
                throw new SecurityException("Privileged WebView administration is not supported");
            default: throw new UnsupportedOperationException("ART WebView update service: "+method.getName());
        }
    }
}
