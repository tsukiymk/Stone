package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ResolveInfo;
import android.content.pm.PackageManager;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageItemInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.PackageInfo;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.content.res.loader.ResourcesLoader;
import android.content.res.loader.ResourcesProvider;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.TypedValue;
import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Map;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/** Owns the resources and launch metadata of one original APK running on ART. */
public final class ArtPackage implements Closeable {
    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final int[] META_DATA_ATTRIBUTES = {android.R.attr.name, android.R.attr.value, android.R.attr.resource};
    private final AssetManager assets;
    private final ResourcesLoader loader;
    private final ResourcesProvider provider;
    private boolean closed;
    private boolean signingLoaded;
    private android.content.pm.SigningInfo signing;
    public final Resources resources;
    public final ApplicationInfo applicationInfo;
    public final ActivityInfo activityInfo;
    public final List<ProviderInfo> providers;
    public final PackageInfo packageInfo;
    final ArtComponentSettings components;
    private final Manifest manifest;
    private static final Map<String,Manifest> manifests = new java.util.LinkedHashMap<String,Manifest>(16,0.75f,true) {
        @Override protected boolean removeEldestEntry(Map.Entry<String,Manifest> entry) { return size() > 128; }
    };

    private ArtPackage(AssetManager assets, ResourcesLoader loader, ResourcesProvider provider,
            Resources resources, ApplicationInfo applicationInfo, ActivityInfo activityInfo,
            List<ProviderInfo> providers, PackageInfo packageInfo, ArtComponentSettings components, Manifest manifest) {
        this.assets = assets;
        this.loader = loader;
        this.provider = provider;
        this.resources = resources;
        this.applicationInfo = applicationInfo;
        this.activityInfo = activityInfo;
        this.providers = Collections.unmodifiableList(providers);
        this.packageInfo = packageInfo;
        this.components = components;
        this.manifest = manifest;
    }

    /** Takes ownership of a new, independent AssetManager supplied by the native ART host. */
    public static ArtPackage open(AssetManager assets, String apk, String dataDirectory) throws IOException {
        return open(assets, apk, dataDirectory, false, false);
    }

    public static ArtPackage openApplication(AssetManager assets, String apk) throws IOException {
        return open(assets, apk, "/data/data", true, false);
    }

    /** Production entry: a trusted older APK remains an alias for its installed update. */
    static ArtPackage openCurrentApplication(AssetManager assets, String apk) throws IOException {
        return open(assets, apk, "/data/data", true, true);
    }

    private static ArtPackage open(AssetManager assets, String apk, String dataDirectory, boolean packageDirectory, boolean followInstalled) throws IOException {
        if (assets == null || apk == null || dataDirectory == null) throw new NullPointerException("ART package inputs");
        ResourcesProvider provider = null;
        ResourcesLoader loader = new ResourcesLoader();
        Resources resources = null;
        boolean loaded = false;
        boolean transferred = false;
        try {
            File source = new File(apk).getCanonicalFile();
            File data = new File(dataDirectory).getCanonicalFile();
            if (!source.isFile()) throw new IOException("APK does not exist: " + source);
            if (!data.getPath().startsWith("/data/")) throw new IOException("Application data must be inside /data");
            try (ParcelFileDescriptor file = ParcelFileDescriptor.open(source, ParcelFileDescriptor.MODE_READ_ONLY)) {
                provider = ResourcesProvider.loadFromApk(file);
            }
            loader.addProvider(provider);
            Resources system = Resources.getSystem();
            resources = new Resources(assets, system.getDisplayMetrics(), system.getConfiguration());
            resources.addLoaders(loader);
            loaded = true;
            Manifest manifest;
            try (XmlResourceParser xml = assets.openXmlResourceParser("AndroidManifest.xml")) {
                manifest = readManifest(xml, resources);
            } catch (XmlPullParserException error) {
                throw new IOException("Invalid APK manifest", error);
            }
            ApplicationInfo application = manifest.application;
            if (!application.packageName.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+"))
                throw new IOException("Invalid application package name: " + application.packageName);
            if (packageDirectory) data = new File(data, application.packageName).getCanonicalFile();
            if (packageDirectory) {
                PackageInfo installed = ArtPackageStore.registered(application.packageName);
                if (followInstalled && installed != null && !source.getPath().equals(installed.applicationInfo.sourceDir)
                        && !source.getPath().startsWith("/data/app/stone/")) {
                    ArtPackageStore.validateLaunchSource(installed,source.getPath(),application.targetSdkVersion,manifest.info.getLongVersionCode());
                    AssetManager replacement;
                    try {
                        java.lang.reflect.Constructor<AssetManager> constructor = AssetManager.class.getDeclaredConstructor();
                        constructor.setAccessible(true); replacement = constructor.newInstance();
                    } catch (ReflectiveOperationException error) { throw new IOException("Cannot create installed application resources",error); }
                    return open(replacement,installed.applicationInfo.sourceDir,dataDirectory,true,true);
                }
                if (installed != null && source.getPath().equals(installed.applicationInfo.sourceDir)) {
                    if (!data.getPath().equals(installed.applicationInfo.dataDir))
                        throw new IOException("Installed application data does not match the package registry");
                    ArtBoot.bindApplicationUid(installed.applicationInfo.uid);
                } else if (source.getPath().startsWith("/data/app/stone/")) {
                    throw new IOException("Managed APK is no longer the registered application version");
                }
            }
            if (!application.enabled) throw new IOException("Application is disabled: " + application.packageName);
            if (application.minSdkVersion > Build.VERSION.SDK_INT)
                throw new IOException("APK requires Android API " + application.minSdkVersion);
            if (packageDirectory) {
                ArtProcess.validate(application, manifest.components.values());
                ArtBoot.lockApplication(application.packageName);
            }
            if (packageDirectory && source.getPath().startsWith("/data/app/stone/")) {
                PackageInfo current = ArtPackageStore.registered(application.packageName);
                if (current == null || !source.getPath().equals(current.applicationInfo.sourceDir))
                    throw new IOException("Managed APK changed before the application lock was acquired");
            }
            if (!data.isDirectory() && !data.mkdirs()) throw new IOException("Cannot create application data: " + data);
            ArtComponentSettings components = new ArtComponentSettings(application.packageName, data, manifest.components);
            ActivityInfo activity = manifest.launcher(components);
            if (activity == null) throw new IOException("APK has no enabled MAIN/LAUNCHER activity");
            application.uid = Binder.getCallingUid();
            application.sourceDir = source.getPath();
            application.publicSourceDir = source.getPath();
            application.dataDir = data.getPath();
            application.deviceProtectedDataDir = new File(data, "device-protected").getPath();
            manifest.providers.sort(Comparator.comparingInt((ProviderInfo info) -> info.initOrder).reversed());
            manifest.info.applicationInfo = application;
            manifest.info.activities = manifest.activities.stream().map(entry -> entry.info).toArray(ActivityInfo[]::new);
            List<ProviderInfo> providers = new ArrayList<>();
            for (ProviderInfo info : manifest.providers) if (components.enabled(info)) providers.add(info);
            manifest.info.providers = providers.toArray(new ProviderInfo[0]);
            manifest.info.receivers = manifest.receivers.toArray(new ActivityInfo[0]);
            manifest.info.services = manifest.serviceEntries.stream().map(entry -> entry.info).toArray(ServiceInfo[]::new);
            manifest.info.requestedPermissions = manifest.permissions.toArray(new String[0]);
            manifest.info.requestedPermissionsFlags = new int[manifest.permissions.size()];
            ArtPackage result = new ArtPackage(assets, loader, provider, resources, application, activity, providers, manifest.info, components, manifest);
            transferred = true;
            return result;
        } finally {
            if (!transferred) {
                if (loaded) resources.removeLoaders(loader);
                loader.clearProviders();
                if (provider != null) provider.close();
                assets.close();
            }
        }
    }

    public Intent launchIntent() {
        if (closed) throw new IllegalStateException("ART package is closed");
        ActivityInfo launcher = launcher();
        if (launcher == null) throw new IllegalStateException("APK has no enabled MAIN/LAUNCHER activity");
        return new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            .setComponent(new ComponentName(applicationInfo.packageName, launcher.name))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    }

    ActivityInfo launcher() {
        List<ActivityInfo> result = queryLaunchers(new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER), 0);
        return result.isEmpty() ? null : result.get(0);
    }

    boolean resizableWindow() {
        ActivityInfo launcher = launcher();
        if (launcher == null) return false;
        try {
            int mode = ActivityInfo.class.getField("resizeMode").getInt(launcher);
            return (Boolean) ActivityInfo.class.getMethod("isResizeableMode", int.class).invoke(null, mode);
        } catch (ReflectiveOperationException error) { throw new IllegalStateException("Unsupported image resize policy", error); }
    }

    List<ActivityInfo> queryLaunchers(Intent intent, long flags) {
        List<ActivityInfo> result = new ArrayList<>();
        if (intent.getComponent() != null || intent.getSelector() != null || intent.getData() != null || intent.getType() != null
                || !Intent.ACTION_MAIN.equals(intent.getAction()) || !intent.hasCategory(Intent.CATEGORY_LAUNCHER)
                || (intent.getPackage() != null && !applicationInfo.packageName.equals(intent.getPackage()))) return result;
        for (String category : intent.getCategories())
            if (!Intent.CATEGORY_LAUNCHER.equals(category) && !Intent.CATEGORY_DEFAULT.equals(category)) return result;
        boolean defaultOnly = (flags & android.content.pm.PackageManager.MATCH_DEFAULT_ONLY) != 0 || intent.hasCategory(Intent.CATEGORY_DEFAULT);
        for (Entry entry : manifest.activities) {
            if (!entry.launcher || (defaultOnly && !entry.launcherDefault)) continue;
            ActivityInfo info = resolveActivity(new Intent().setComponent(new ComponentName(applicationInfo.packageName, entry.info.name)));
            if (info != null) result.add(info);
        }
        return result;
    }

    static final class FilterMatch {
        final String name;
        final IntentFilter filter;
        final int score;
        FilterMatch(String name, IntentFilter filter, int score) { this.name = name; this.filter = filter; this.score = score; }
        ResolveInfo resolve(ActivityInfo info, long flags) {
            ResolveInfo result = new ResolveInfo(); result.activityInfo = info;
            result.priority = filter.getPriority(); result.match = score; result.isDefault = filter.hasCategory(Intent.CATEGORY_DEFAULT);
            if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) result.filter = new IntentFilter(filter);
            return result;
        }
    }
    private static List<FilterMatch> matchingFilters(Manifest manifest, Intent intent, long flags) {
        return matchingFilters(manifest,manifest.activities,intent,flags);
    }
    private static List<FilterMatch> matchingFilters(Manifest manifest, List<? extends ComponentEntry<?>> entries, Intent intent, long flags) {
        List<FilterMatch> matches = new ArrayList<>();
        if (intent == null || intent.getSelector() != null || intent.getComponent() != null
                || intent.getPackage() != null && !manifest.application.packageName.equals(intent.getPackage())) return matches;
        for (ComponentEntry<?> entry : entries) {
            FilterMatch best = null;
            for (IntentFilter filter : entry.filters) {
                if ((flags & PackageManager.MATCH_DEFAULT_ONLY) != 0 && !filter.hasCategory(Intent.CATEGORY_DEFAULT)) continue;
                int score = filter.match(intent.getAction(),intent.getType(),intent.getScheme(),intent.getData(),intent.getCategories(),"StonePackages");
                if (score >= 0 && (best == null || filter.getPriority() > best.filter.getPriority()
                        || filter.getPriority() == best.filter.getPriority() && score > best.score)) best = new FilterMatch(entry.info.name,filter,score);
            }
            if (best != null) matches.add(best);
        }
        matches.sort(java.util.Comparator.<FilterMatch>comparingInt(value -> value.filter.getPriority()).reversed()
            .thenComparing(java.util.Comparator.comparingInt((FilterMatch value) -> value.score).reversed()));
        return matches;
    }
    List<ResolveInfo> queryActivities(Intent intent, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent == null || intent.getSelector() != null) return result;
        if (intent.getComponent() != null) {
            if (intent.getPackage() != null && !intent.getPackage().equals(intent.getComponent().getPackageName())) return result;
            ActivityInfo info = activity(intent.getComponent(),flags);
            if (info != null) { ResolveInfo resolved = new ResolveInfo(); resolved.activityInfo = info; result.add(resolved); }
            return result;
        }
        for (FilterMatch match : matchingFilters(manifest,intent,flags)) {
            ActivityInfo info = activity(new ComponentName(applicationInfo.packageName,match.name),flags);
            if (info != null) result.add(match.resolve(info,flags));
        }
        return result;
    }
    List<ResolveInfo> queryReceivers(Intent intent, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent == null || intent.getSelector() != null || intent.getPackage() != null
                && !applicationInfo.packageName.equals(intent.getPackage())) return result;
        List<FilterMatch> matches=matchingFilters(manifest,manifest.receiverEntries,intent,flags);
        for (ActivityInfo info : packageInfo.receivers) {
            boolean enabled = applicationInfo.enabled && components.enabled(info);
            if (!enabled && (flags & PackageManager.MATCH_DISABLED_COMPONENTS) == 0) continue;
            if (intent.getComponent() != null) {
                if (!intent.getComponent().equals(new ComponentName(applicationInfo.packageName,info.name))) continue;
                ResolveInfo resolved = new ResolveInfo(); resolved.activityInfo = receiverCopy(info,flags,enabled); result.add(resolved);
            } else for (FilterMatch match : matches) {
                if (match.name.equals(info.name)) result.add(match.resolve(receiverCopy(info,flags,enabled),flags));
            }
        }
        result.sort(java.util.Comparator.comparingInt((ResolveInfo info) -> info.priority).reversed()
            .thenComparing(java.util.Comparator.comparingInt((ResolveInfo info) -> info.match).reversed()));
        return result;
    }
    List<ResolveInfo> queryServices(Intent intent, long flags) {
        List<ResolveInfo> result=new ArrayList<>();
        if(intent==null || intent.getSelector()!=null || intent.getPackage()!=null && !applicationInfo.packageName.equals(intent.getPackage())) return result;
        List<FilterMatch> matches=matchingFilters(manifest,manifest.serviceEntries,intent,flags);
        for(ServiceInfo info:packageInfo.services) {
            boolean enabled=applicationInfo.enabled && components.enabled(info);
            if(!enabled && (flags & PackageManager.MATCH_DISABLED_COMPONENTS)==0) continue;
            if(intent.getComponent()!=null && !intent.getComponent().equals(new ComponentName(applicationInfo.packageName,info.name))) continue;
            FilterMatch match=null;
            if(intent.getComponent()==null) {
                for(FilterMatch candidate:matches) if(candidate.name.equals(info.name)) { match=candidate; break; }
                if(match==null) continue;
            }
            ResolveInfo resolved=new ResolveInfo(); resolved.serviceInfo=new ServiceInfo(info);
            resolved.serviceInfo.applicationInfo=new ApplicationInfo(applicationInfo); resolved.serviceInfo.enabled=enabled;
            resolved.serviceInfo.metaData=(flags & PackageManager.GET_META_DATA)!=0 && info.metaData!=null ? new Bundle(info.metaData) : null;
            if(match!=null) {
                resolved.priority=match.filter.getPriority(); resolved.match=match.score; resolved.isDefault=match.filter.hasCategory(Intent.CATEGORY_DEFAULT);
                if((flags & PackageManager.GET_RESOLVED_FILTER)!=0) resolved.filter=new IntentFilter(match.filter);
            }
            result.add(resolved);
        }
        result.sort(java.util.Comparator.comparingInt((ResolveInfo info) -> info.priority).reversed()
            .thenComparing(java.util.Comparator.comparingInt((ResolveInfo info) -> info.match).reversed()));
        return result;
    }
    static ActivityInfo receiverCopy(ActivityInfo info, long flags, boolean enabled) {
        ActivityInfo result = new ActivityInfo(info);
        result.applicationInfo = new ApplicationInfo(info.applicationInfo);
        result.enabled = enabled;
        result.metaData = (flags & PackageManager.GET_META_DATA) != 0 && info.metaData != null ? new Bundle(info.metaData) : null;
        return result;
    }
    static List<FilterMatch> installedMatches(PackageInfo info, Intent intent, long flags) throws IOException {
        return matchingFilters(installedManifest(info),intent,flags);
    }

    /** Read launch filters during installation/migration, without creating data or taking an app lock. */
    static String[] launcherNames(PackageInfo info) throws IOException {
        Manifest manifest = installedManifest(info);
        List<String> names = new ArrayList<>();
        for (Entry entry : manifest.activities) if (entry.launcher) names.add(entry.info.name + (entry.launcherDefault ? "\tdefault" : ""));
        return names.toArray(new String[0]);
    }
    private static Manifest installedManifest(PackageInfo info) throws IOException {
        File code = new File(info.applicationInfo.sourceDir);
        String identity = info.packageName+"\n"+code.getCanonicalPath()+"\n"+code.length()+":"+code.lastModified();
        synchronized (manifests) { Manifest cached = manifests.get(identity); if (cached != null) return cached; }
        AssetManager assets;
        try {
            java.lang.reflect.Constructor<AssetManager> constructor = AssetManager.class.getDeclaredConstructor();
            constructor.setAccessible(true);
            assets = constructor.newInstance();
        } catch (ReflectiveOperationException error) { throw new IOException("Cannot create manifest resources", error); }
        ResourcesLoader loader = new ResourcesLoader();
        ResourcesProvider provider = null;
        Resources resources = null;
        try {
            try (ParcelFileDescriptor file = ParcelFileDescriptor.open(new File(info.applicationInfo.sourceDir), ParcelFileDescriptor.MODE_READ_ONLY)) {
                provider = ResourcesProvider.loadFromApk(file);
            }
            loader.addProvider(provider);
            Resources system = Resources.getSystem();
            resources = new Resources(assets, system.getDisplayMetrics(), system.getConfiguration());
            resources.addLoaders(loader);
            Manifest manifest;
            try (XmlResourceParser xml = assets.openXmlResourceParser("AndroidManifest.xml")) {
                manifest = readManifest(xml, resources);
            } catch (XmlPullParserException error) { throw new IOException("Invalid installed APK manifest", error); }
            if (!info.packageName.equals(manifest.application.packageName)) throw new IOException("Installed manifest identity changed");
            String after = info.packageName+"\n"+code.getCanonicalPath()+"\n"+code.length()+":"+code.lastModified();
            if (!identity.equals(after)) throw new IOException("Installed manifest changed during query");
            synchronized (manifests) { manifests.put(identity,manifest); }
            return manifest;
        } finally {
            if (resources != null) resources.removeLoaders(loader);
            loader.clearProviders();
            if (provider != null) provider.close();
            assets.close();
        }
    }

    public String activityClassName() {
        return activityInfo.targetActivity == null ? activityInfo.name : activityInfo.targetActivity;
    }

    synchronized PackageInfo packageInfo(long flags) {
        boolean certificates = (flags & (android.content.pm.PackageManager.GET_SIGNATURES
            | android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES)) != 0;
        if (certificates && !signingLoaded) {
            try { signing = ArtArchive.verify(applicationInfo.sourceDir, applicationInfo.targetSdkVersion); }
            catch (IOException error) { android.util.Log.w("StonePackage", "Launched APK has no verified signing identity", error); }
            signingLoaded = true;
        }
        android.os.Parcel parcel = android.os.Parcel.obtain();
        PackageInfo result;
        try {
            packageInfo.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            result = PackageInfo.CREATOR.createFromParcel(parcel);
        } finally { parcel.recycle(); }
        if (signing != null) {
            if ((flags & android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES) != 0)
                result.signingInfo = new android.content.pm.SigningInfo(signing);
            if ((flags & android.content.pm.PackageManager.GET_SIGNATURES) != 0)
                result.signatures = signing.hasPastSigningCertificates()
                    ? new android.content.pm.Signature[]{signing.getSigningCertificateHistory()[0]}
                    : signing.getApkContentsSigners().clone();
        }
        return result;
    }

    ActivityInfo resolveActivity(Intent intent) {
        return resolveActivity(intent,0);
    }
    ActivityInfo resolveActivity(Intent intent, long flags) {
        ComponentName component = intent.getComponent();
        if (component == null) {
            List<ResolveInfo> matches = queryActivities(intent, flags);
            return matches.isEmpty() ? null : matches.get(0).activityInfo;
        }
        if (intent.getPackage() != null && !intent.getPackage().equals(component.getPackageName())) return null;
        return activity(component,flags);
    }
    private ActivityInfo activity(ComponentName component, long flags) {
        if (!applicationInfo.packageName.equals(component.getPackageName())) return null;
        for (ActivityInfo info : packageInfo.activities) {
            if (!info.name.equals(component.getClassName())) continue;
            boolean enabled = applicationInfo.enabled && components.enabled(info);
            if (!enabled && (flags & PackageManager.MATCH_DISABLED_COMPONENTS) == 0) return null;
            if (info.targetActivity == null) {
                ActivityInfo resolved = new ActivityInfo(info); resolved.enabled = enabled; return resolved;
            }
            for (ActivityInfo candidate : packageInfo.activities) {
                if (candidate.name.equals(info.targetActivity) && candidate.targetActivity == null) {
                    enabled &= components.enabled(candidate);
                    if (!enabled && (flags & PackageManager.MATCH_DISABLED_COMPONENTS) == 0) return null;
                    ActivityInfo resolved = aliasInfo(info,candidate); resolved.enabled = enabled; return resolved;
                }
            }
        }
        return null;
    }

    private static ActivityInfo aliasInfo(ActivityInfo info, ActivityInfo target) {
        ActivityInfo alias = new ActivityInfo(target);
        alias.name = info.name;
        alias.targetActivity = info.targetActivity;
        alias.enabled = true; // Only called after effective component-state checks.
        alias.exported = info.exported;
        alias.permission = info.permission == null ? target.permission : info.permission.isEmpty() ? null : info.permission;
        alias.icon = info.icon;
        alias.labelRes = info.labelRes;
        alias.nonLocalizedLabel = info.nonLocalizedLabel;
        alias.metaData = info.metaData == null ? null : new Bundle(info.metaData);
        return alias;
    }

    List<ProviderInfo> localProviders() {
        List<ProviderInfo> local = new ArrayList<>();
        for (ProviderInfo info : providers) {
            if (ArtProcess.current(this).equals(info.processName) && components.enabled(info)) local.add(info);
        }
        return local;
    }

    ActivityInfo receiver(ComponentName name) {
        if (!applicationInfo.packageName.equals(name.getPackageName())) return null;
        for (ActivityInfo receiver : packageInfo.receivers) if (receiver.name.equals(name.getClassName()) && components.enabled(receiver)) {
            if (!ArtProcess.current(this).equals(receiver.processName))
                throw new UnsupportedOperationException("Cross-process APK receivers are not connected yet");
            return receiver;
        }
        return null;
    }

    @Override public void close() {
        if (closed) return;
        closed = true;
        resources.removeLoaders(loader);
        loader.clearProviders();
        provider.close();
        assets.close();
    }

    private static class ComponentEntry<T extends ComponentInfo> {
        T info;
        final List<IntentFilter> filters = new ArrayList<>();
        boolean exportedSpecified, intentFilter;
    }
    private static final class Entry extends ComponentEntry<ActivityInfo> {
        boolean launcher;
        boolean launcherDefault;
    }
    private static final class Manifest {
        final ApplicationInfo application = new ApplicationInfo();
        final PackageInfo info = new PackageInfo();
        final List<Entry> activities = new ArrayList<>();
        final List<ProviderInfo> providers = new ArrayList<>();
        final List<ActivityInfo> receivers = new ArrayList<>();
        final List<Entry> receiverEntries = new ArrayList<>();
        final List<ComponentEntry<ServiceInfo>> serviceEntries = new ArrayList<>();
        final Map<String, ComponentInfo> components = new HashMap<>();
        final List<String> permissions = new ArrayList<>();
        String permission;
        ActivityInfo launcher(ArtComponentSettings settings) throws IOException {
            for (Entry entry : activities) {
                ActivityInfo info = entry.info;
                if (!entry.launcher || !settings.enabled(info)) continue;
                if (info.targetActivity != null) {
                    ActivityInfo target = null;
                    for (Entry candidate : activities) {
                        if (candidate.info.targetActivity == null && candidate.info.name.equals(info.targetActivity)) {
                            target = candidate.info;
                            break;
                        }
                    }
                    if (target == null) throw new IOException("Activity alias target is missing: " + info.targetActivity);
                    if (!settings.enabled(target)) continue;
                    return aliasInfo(info, target);
                }
                return info;
            }
            return null;
        }
    }

    private static String className(String name, String packageName) {
        if (name == null || name.isEmpty()) return null;
        return name.charAt(0) == '.' ? packageName + name : name.indexOf('.') < 0 ? packageName + '.' + name : name;
    }
    private static String processName(String name, String defaultName, String packageName) {
        if (name == null || name.isEmpty()) return defaultName;
        return name.charAt(0) == ':' ? packageName + name : name;
    }
    private static int sdk(XmlResourceParser xml, String name, int fallback) throws IOException {
        String raw = xml.getAttributeValue(ANDROID, name);
        if (raw != null && !raw.isEmpty() && Character.isLetter(raw.charAt(0))) {
            if ("REL".equals(Build.VERSION.CODENAME) || !raw.equals(Build.VERSION.CODENAME))
                throw new IOException("Unsupported APK SDK codename: " + raw);
            return Build.VERSION.SDK_INT;
        }
        return xml.getAttributeIntValue(ANDROID, name, fallback);
    }
    private static void readPresentation(XmlResourceParser xml, PackageItemInfo item) {
        item.labelRes = xml.getAttributeResourceValue(ANDROID, "label", 0);
        item.nonLocalizedLabel = item.labelRes == 0 ? xml.getAttributeValue(ANDROID, "label") : null;
        item.icon = xml.getAttributeResourceValue(ANDROID, "icon", 0);
    }

    private static boolean manifestBoolean(XmlResourceParser xml, Resources resources, String name, boolean fallback) {
        int resource = xml.getAttributeResourceValue(ANDROID, name, 0);
        return resource != 0 ? resources.getBoolean(resource) : xml.getAttributeBooleanValue(ANDROID, name, fallback);
    }

    private static boolean enabled(XmlResourceParser xml, Resources resources) {
        return manifestBoolean(xml, resources, "enabled", true);
    }

    private static void setBackCallbackFlag(Object info, Class<?> type, String field, String flag) throws IOException {
        try {
            java.lang.reflect.Field flags = type.getField(field);
            flags.setInt(info, flags.getInt(info) | type.getField(flag).getInt(null));
        } catch (ReflectiveOperationException error) {
            throw new IOException("Could not set " + type.getSimpleName() + " back navigation flag", error);
        }
    }

    private static void readMetaData(XmlResourceParser xml, Resources resources, PackageItemInfo item) throws IOException {
        TypedArray attributes = resources.obtainAttributes(xml, META_DATA_ATTRIBUTES);
        try {
            String name = attributes.getString(0);
            if (name == null) throw new IOException("Manifest meta-data is missing android:name");
            int resource = attributes.getResourceId(2, 0);
            TypedValue value = attributes.peekValue(1);
            if (resource == 0 && value == null) throw new IOException("Manifest meta-data is missing a value: " + name);
            if (item.metaData == null) item.metaData = new Bundle();
            if (resource != 0) item.metaData.putInt(name, resource);
            else if (value.type == TypedValue.TYPE_STRING) item.metaData.putString(name, value.coerceToString().toString());
            else if (value.type == TypedValue.TYPE_INT_BOOLEAN) item.metaData.putBoolean(name, value.data != 0);
            else if (value.type >= TypedValue.TYPE_FIRST_INT && value.type <= TypedValue.TYPE_LAST_INT)
                item.metaData.putInt(name, value.data);
            else if (value.type == TypedValue.TYPE_FLOAT) item.metaData.putFloat(name, value.getFloat());
        } finally {
            attributes.recycle();
        }
    }

    private static String filterString(XmlResourceParser xml, Resources resources, String name) {
        int resource = xml.getAttributeResourceValue(ANDROID,name,0);
        return resource == 0 ? xml.getAttributeValue(ANDROID,name) : resources.getString(resource);
    }
    private static boolean filterElement(IntentFilter filter, XmlResourceParser xml, Resources resources) throws IOException {
        String tag = xml.getName();
        if (tag.equals("action") || tag.equals("category")) {
            String name = filterString(xml,resources,"name"); if (name == null || name.isEmpty()) return false;
            if (tag.equals("action")) filter.addAction(name); else filter.addCategory(name); return true;
        }
        if (!tag.equals("data")) return false; // Unsupported relative URI groups fail closed.
        java.util.Set<String> supported = new java.util.HashSet<>(java.util.Arrays.asList("mimeType","scheme","host","port",
            "path","pathPrefix","pathPattern","pathAdvancedPattern","pathSuffix","ssp","sspPrefix","sspPattern","sspAdvancedPattern","sspSuffix"));
        for (int index = 0; index < xml.getAttributeCount(); ++index)
            if (ANDROID.equals(xml.getAttributeNamespace(index)) && !supported.contains(xml.getAttributeName(index))) return false;
        String type = filterString(xml,resources,"mimeType"), scheme = filterString(xml,resources,"scheme"), host = filterString(xml,resources,"host");
        if (type != null) try { filter.addDataType(type); }
        catch (IntentFilter.MalformedMimeTypeException error) { throw new IOException("Invalid Activity filter MIME type",error); }
        if (scheme != null) filter.addDataScheme(scheme);
        if (host != null) filter.addDataAuthority(host,filterString(xml,resources,"port"));
        String[] paths = {"path","pathPrefix","pathPattern","pathAdvancedPattern","pathSuffix"};
        String[] parts = {"ssp","sspPrefix","sspPattern","sspAdvancedPattern","sspSuffix"};
        int[] patterns = {android.os.PatternMatcher.PATTERN_LITERAL,android.os.PatternMatcher.PATTERN_PREFIX,
            android.os.PatternMatcher.PATTERN_SIMPLE_GLOB,android.os.PatternMatcher.PATTERN_ADVANCED_GLOB,android.os.PatternMatcher.PATTERN_SUFFIX};
        for (int index = 0; index < patterns.length; ++index) {
            String path = filterString(xml,resources,paths[index]), part = filterString(xml,resources,parts[index]);
            if (path != null) filter.addDataPath(path,patterns[index]);
            if (part != null) filter.addDataSchemeSpecificPart(part,patterns[index]);
        }
        return true;
    }
    private static Manifest readManifest(XmlResourceParser xml, Resources resources) throws IOException, XmlPullParserException {
        Manifest result = new Manifest();
        ApplicationInfo application = result.application;
        application.enabled = true;
        application.minSdkVersion = 1;
        application.targetSdkVersion = 1;
        int applicationDepth = -1, activityDepth = -1, serviceDepth = -1, filterDepth = -1;
        Entry entry = null;
        ComponentEntry<ServiceInfo> serviceEntry = null;
        ComponentInfo component = null;
        boolean hasApplication = false, filterSupported = true;
        Boolean applicationResizable = null;
        IntentFilter filter = null;
        int event;
        while ((event = xml.next()) != XmlPullParser.END_DOCUMENT) {
            int depth = xml.getDepth();
            if (event == XmlPullParser.END_TAG) {
                if (depth == filterDepth) {
                    if (filterSupported && filter.countActions() != 0) {
                        (entry != null ? entry : serviceEntry).filters.add(filter);
                        if (entry != null) {
                            boolean launcher = filter.match(Intent.ACTION_MAIN,null,null,null,Collections.singleton(Intent.CATEGORY_LAUNCHER),"StonePackages") >= 0;
                            entry.launcher |= launcher;
                            entry.launcherDefault |= launcher && filter.hasCategory(Intent.CATEGORY_DEFAULT);
                        }
                    }
                    filterDepth = -1;
                    filter = null;
                }
                if (depth == activityDepth) {
                    if (entry.intentFilter && !entry.exportedSpecified && application.targetSdkVersion >= 31)
                        throw new IOException("Activity with intent filters must declare exported: " + entry.info.name);
                    entry = null;
                    activityDepth = -1;
                }
                if (depth == serviceDepth) {
                    if (serviceEntry.intentFilter && !serviceEntry.exportedSpecified && application.targetSdkVersion >= 31)
                        throw new IOException("Service with intent filters must declare exported: " + serviceEntry.info.name);
                    serviceEntry = null; serviceDepth = -1;
                }
                if (depth == applicationDepth) applicationDepth = -1;
                if (component != null && depth == applicationDepth + 1) component = null;
                continue;
            }
            if (event != XmlPullParser.START_TAG) continue;
            String tag = xml.getName();
            if (depth == 1) {
                if (!"manifest".equals(tag)) throw new IOException("APK root is not manifest");
                application.packageName = xml.getAttributeValue(null, "package");
                if (application.packageName == null || application.packageName.isEmpty()) throw new IOException("APK package name is missing");
                result.info.packageName = application.packageName;
                long version = (xml.getAttributeIntValue(ANDROID, "versionCodeMajor", 0) & 0xffffffffL) << 32;
                version |= xml.getAttributeIntValue(ANDROID, "versionCode", 0) & 0xffffffffL;
                result.info.setLongVersionCode(version);
                int versionName = xml.getAttributeResourceValue(ANDROID, "versionName", 0);
                result.info.versionName = versionName == 0 ? xml.getAttributeValue(ANDROID, "versionName") : resources.getString(versionName);
                String split = xml.getAttributeValue(null, "split");
                if (split != null && !split.isEmpty()) throw new IOException("Expected a base APK, received split " + split);
                application.processName = application.packageName;
            } else if (depth == 2 && "uses-sdk".equals(tag)) {
                application.minSdkVersion = sdk(xml, "minSdkVersion", 1);
                application.targetSdkVersion = sdk(xml, "targetSdkVersion", application.minSdkVersion);
            } else if (depth == 2 && ("uses-permission".equals(tag) || "uses-permission-sdk-23".equals(tag))) {
                String permission = xml.getAttributeValue(ANDROID, "name");
                if (permission != null && Build.VERSION.SDK_INT <= xml.getAttributeIntValue(ANDROID, "maxSdkVersion", Integer.MAX_VALUE)
                        && !result.permissions.contains(permission)) result.permissions.add(permission);
            } else if (depth == 2 && "application".equals(tag)) {
                if (hasApplication) throw new IOException("APK contains multiple application tags");
                hasApplication = true;
                applicationDepth = depth;
                if (xml.getAttributeValue(ANDROID, "resizeableActivity") != null)
                    applicationResizable = manifestBoolean(xml, resources, "resizeableActivity", false);
                application.className = className(xml.getAttributeValue(ANDROID, "name"), application.packageName);
                application.processName = processName(xml.getAttributeValue(ANDROID, "process"), application.packageName, application.packageName);
                result.permission = xml.getAttributeValue(ANDROID, "permission");
                application.appComponentFactory = className(xml.getAttributeValue(ANDROID, "appComponentFactory"), application.packageName);
                application.enabled = enabled(xml, resources);
                application.theme = xml.getAttributeResourceValue(ANDROID, "theme", 0);
                if (Build.VERSION.SDK_INT >= 33) try {
                    ApplicationInfo.class.getMethod("setLocaleConfigRes", int.class).invoke(application,
                        xml.getAttributeResourceValue(ANDROID, "localeConfig", 0));
                } catch (ReflectiveOperationException error) { throw new IOException("Could not preserve APK locale configuration", error); }
                application.flags = xml.getAttributeBooleanValue(ANDROID, "hasCode", true) ? ApplicationInfo.FLAG_HAS_CODE : 0;
                if (xml.getAttributeBooleanValue(ANDROID, "debuggable", false)) application.flags |= ApplicationInfo.FLAG_DEBUGGABLE;
                if (xml.getAttributeBooleanValue(ANDROID, "usesCleartextTraffic", application.targetSdkVersion < 28))
                    application.flags |= ApplicationInfo.FLAG_USES_CLEARTEXT_TRAFFIC;
                if (Build.VERSION.SDK_INT >= 33 && manifestBoolean(xml, resources, "enableOnBackInvokedCallback",
                        Build.VERSION.SDK_INT >= 36 && application.targetSdkVersion >= 36))
                    setBackCallbackFlag(application, ApplicationInfo.class, "privateFlagsExt",
                        "PRIVATE_FLAG_EXT_ENABLE_ON_BACK_INVOKED_CALLBACK");
                readPresentation(xml, application);
            } else if (applicationDepth > 0 && depth == applicationDepth + 1 && "provider".equals(tag)) {
                ProviderInfo info = new ProviderInfo();
                info.applicationInfo = application;
                info.packageName = application.packageName;
                info.processName = processName(xml.getAttributeValue(ANDROID, "process"), application.processName, application.packageName);
                info.name = className(xml.getAttributeValue(ANDROID, "name"), application.packageName);
                info.authority = xml.getAttributeValue(ANDROID, "authorities");
                if (info.name == null || info.authority == null || info.authority.isEmpty())
                    throw new IOException("Provider name or authorities are missing");
                info.enabled = enabled(xml, resources);
                info.exported = xml.getAttributeBooleanValue(ANDROID, "exported", application.targetSdkVersion < 17);
                info.initOrder = xml.getAttributeIntValue(ANDROID, "initOrder", 0);
                String permission = xml.getAttributeValue(ANDROID, "permission");
                if (permission == null) permission = result.permission;
                info.readPermission = xml.getAttributeValue(ANDROID, "readPermission");
                if (info.readPermission == null) info.readPermission = permission;
                info.writePermission = xml.getAttributeValue(ANDROID, "writePermission");
                if (info.writePermission == null) info.writePermission = permission;
                info.grantUriPermissions = xml.getAttributeBooleanValue(ANDROID, "grantUriPermissions", false);
                result.providers.add(info);
                result.components.put(info.name, info);
                component = info;
            } else if (applicationDepth > 0 && depth == applicationDepth + 1 && ("service".equals(tag) || "receiver".equals(tag))) {
                ComponentInfo info = "service".equals(tag) ? new ServiceInfo() : new ActivityInfo();
                info.applicationInfo = application;
                info.packageName = application.packageName;
                info.name = className(xml.getAttributeValue(ANDROID, "name"), application.packageName);
                if (info.name == null) throw new IOException("Component name is missing: " + tag);
                info.processName = processName(xml.getAttributeValue(ANDROID, "process"), application.processName, application.packageName);
                info.enabled = enabled(xml, resources);
                info.exported = xml.getAttributeBooleanValue(ANDROID, "exported", false);
                if (info instanceof ServiceInfo) {
                    ServiceInfo service = (ServiceInfo) info;
                    serviceEntry = new ComponentEntry<>(); serviceEntry.info = service;
                    serviceEntry.exportedSpecified = xml.getAttributeValue(ANDROID,"exported") != null;
                    result.serviceEntries.add(serviceEntry); serviceDepth = depth;
                    service.permission = xml.getAttributeValue(ANDROID, "permission");
                    try { ServiceInfo.class.getField("mForegroundServiceType").setInt(service,
                        xml.getAttributeIntValue(ANDROID, "foregroundServiceType", 0)); }
                    catch (ReflectiveOperationException error) { throw new IOException("Unsupported image ServiceInfo", error); }
                    if (xml.getAttributeBooleanValue(ANDROID, "isolatedProcess", false)) service.flags |= ServiceInfo.FLAG_ISOLATED_PROCESS;
                }
                else ((ActivityInfo) info).permission = xml.getAttributeValue(ANDROID, "permission");
                if ("receiver".equals(tag)) {
                    result.receivers.add((ActivityInfo) info);
                    entry = new Entry(); entry.info = (ActivityInfo)info;
                    entry.exportedSpecified = xml.getAttributeValue(ANDROID,"exported") != null;
                    result.receiverEntries.add(entry); activityDepth = depth;
                }
                result.components.put(info.name, info);
                component = info;
            } else if ("meta-data".equals(tag) && applicationDepth > 0 && depth == applicationDepth + 1) {
                readMetaData(xml, resources, application);
            } else if ("meta-data".equals(tag) && component != null && depth == applicationDepth + 2) {
                readMetaData(xml, resources, component);
            } else if ("meta-data".equals(tag) && entry != null && depth == activityDepth + 1) {
                readMetaData(xml, resources, entry.info);
            } else if (applicationDepth > 0 && depth == applicationDepth + 1 && ("activity".equals(tag) || "activity-alias".equals(tag))) {
                entry = new Entry();
                entry.info = new ActivityInfo();
                entry.info.applicationInfo = application;
                entry.info.packageName = application.packageName;
                entry.info.processName = processName(xml.getAttributeValue(ANDROID, "process"), application.processName, application.packageName);
                entry.info.launchMode = xml.getAttributeIntValue(ANDROID, "launchMode", ActivityInfo.LAUNCH_MULTIPLE);
                entry.info.permission = xml.getAttributeValue(ANDROID, "permission");
                if (!"activity-alias".equals(tag)) {
                    if (entry.info.permission == null) entry.info.permission = result.permission;
                    if ("".equals(entry.info.permission)) entry.info.permission = null;
                }
                entry.info.name = className(xml.getAttributeValue(ANDROID, "name"), application.packageName);
                if (entry.info.name == null) throw new IOException("Activity name is missing");
                entry.info.targetActivity = "activity-alias".equals(tag)
                    ? className(xml.getAttributeValue(ANDROID, "targetActivity"), application.packageName) : null;
                if ("activity-alias".equals(tag) && entry.info.targetActivity == null) throw new IOException("Activity alias target is missing");
                entry.info.enabled = enabled(xml, resources);
                entry.info.exported = xml.getAttributeBooleanValue(ANDROID, "exported", false);
                entry.exportedSpecified = xml.getAttributeValue(ANDROID, "exported") != null;
                entry.info.theme = xml.getAttributeResourceValue(ANDROID, "theme", 0);
                entry.info.screenOrientation = xml.getAttributeIntValue(ANDROID, "screenOrientation", ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
                Boolean resizable = xml.getAttributeValue(ANDROID, "resizeableActivity") == null ? applicationResizable
                    : Boolean.valueOf(manifestBoolean(xml, resources, "resizeableActivity", false));
                try {
                    String mode = resizable != null ? (resizable ? "RESIZE_MODE_RESIZEABLE" : "RESIZE_MODE_UNRESIZEABLE")
                        : (application.targetSdkVersion >= 24 ? "RESIZE_MODE_RESIZEABLE_VIA_SDK_VERSION" : "RESIZE_MODE_UNRESIZEABLE");
                    ActivityInfo.class.getField("resizeMode").setInt(entry.info, ActivityInfo.class.getField(mode).getInt(null));
                } catch (ReflectiveOperationException error) { throw new IOException("Unsupported image resize policy", error); }
                entry.info.configChanges = xml.getAttributeIntValue(ANDROID, "configChanges", 0);
                if (Build.VERSION.SDK_INT >= 33 && entry.info.targetActivity == null
                        && xml.getAttributeValue(ANDROID, "enableOnBackInvokedCallback") != null) {
                    boolean backCallback = manifestBoolean(xml, resources, "enableOnBackInvokedCallback", false);
                    setBackCallbackFlag(entry.info, ActivityInfo.class, "privateFlags", backCallback
                        ? "PRIVATE_FLAG_ENABLE_ON_BACK_INVOKED_CALLBACK" : "PRIVATE_FLAG_DISABLE_ON_BACK_INVOKED_CALLBACK");
                }
                readPresentation(xml, entry.info);
                result.activities.add(entry);
                result.components.put(entry.info.name, entry.info);
                activityDepth = depth;
            } else if ((entry != null && depth == activityDepth + 1 || serviceEntry != null && depth == serviceDepth + 1) && "intent-filter".equals(tag)) {
                filterDepth = depth;
                ComponentEntry<?> owner = entry != null ? entry : serviceEntry;
                owner.intentFilter = true;
                if (!owner.exportedSpecified) owner.info.exported = true;
                filter = new IntentFilter(); filterSupported = true;
                // Every APK here is an ordinary application, not a privileged system package.
                filter.setPriority(Math.min(0,xml.getAttributeIntValue(ANDROID,"priority",0)));
            } else if (filterDepth > 0 && depth == filterDepth + 1) {
                filterSupported &= filterElement(filter,xml,resources);
            }
        }
        if (!hasApplication) throw new IOException("APK application tag is missing");
        return result;
    }
}
