package com.trebledot.stone.runtime;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.SigningInfo;
import android.os.Build;
import java.io.File;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;

/** APK metadata and full-content signature verification supplied by the selected Android image. */
public final class ArtArchive {
    private ArtArchive() {}

    static String labelInLocale(PackageInfo info, String languageTag) throws IOException {
        android.content.pm.ApplicationInfo app = info.applicationInfo;
        if (app.nonLocalizedLabel != null) return app.nonLocalizedLabel.toString().trim();
        if (app.labelRes == 0) return app.name == null ? info.packageName : app.name;
        return withResources(info, languageTag, resources -> resources.getText(app.labelRes).toString().trim());
    }

    static <T> T withResources(PackageInfo info, String languageTag, java.util.function.Function<android.content.res.Resources,T> reader) throws IOException {
        android.content.pm.ApplicationInfo app = info.applicationInfo;
        android.content.res.AssetManager assets;
        try {
            java.lang.reflect.Constructor<android.content.res.AssetManager> constructor = android.content.res.AssetManager.class.getDeclaredConstructor();
            constructor.setAccessible(true);
            assets = constructor.newInstance();
        } catch (ReflectiveOperationException error) { throw new IOException("Cannot create APK label resources", error); }
        android.content.res.loader.ResourcesLoader loader = new android.content.res.loader.ResourcesLoader();
        android.content.res.loader.ResourcesProvider provider = null;
        android.content.res.Resources resources = null;
        try {
            try (android.os.ParcelFileDescriptor file = android.os.ParcelFileDescriptor.open(new File(app.sourceDir), android.os.ParcelFileDescriptor.MODE_READ_ONLY)) {
                provider = android.content.res.loader.ResourcesProvider.loadFromApk(file);
            }
            loader.addProvider(provider);
            android.content.res.Resources system = android.content.res.Resources.getSystem();
            android.content.res.Configuration config = new android.content.res.Configuration(system.getConfiguration());
            config.setLocales(android.os.LocaleList.forLanguageTags(languageTag));
            resources = new android.content.res.Resources(assets, system.getDisplayMetrics(), config);
            resources.addLoaders(loader);
            return reader.apply(resources);
        } catch (android.content.res.Resources.NotFoundException error) { throw new IOException("Cannot read the approved APK label", error); }
        finally {
            if (resources != null) resources.removeLoaders(loader);
            if (provider != null) { loader.removeProvider(provider); provider.close(); }
            assets.close();
        }
    }

    public static PackageInfo inspect(Context context, String path) throws IOException {
        return inspect(context.getPackageManager(), path);
    }

    static PackageInfo inspect(PackageManager manager, String path) throws IOException {
        File file = new File(path).getCanonicalFile();
        if (!file.isFile()) throw new IOException("APK does not exist: " + file);
        int flags = PackageManager.GET_ACTIVITIES | PackageManager.GET_RECEIVERS | PackageManager.GET_SERVICES
            | PackageManager.GET_PROVIDERS | PackageManager.GET_PERMISSIONS | PackageManager.GET_META_DATA
            | PackageManager.MATCH_DISABLED_COMPONENTS;
        PackageInfo info = manager.getPackageArchiveInfo(file.getPath(), flags);
        if (info == null || info.applicationInfo == null) throw new IOException("Invalid or unsupported APK manifest: " + file);
        ArtPackageCompatibility.checkSdk(info.applicationInfo.minSdkVersion, Build.VERSION.SDK_INT);
        info.signingInfo = verify(file.getPath(), info.applicationInfo.targetSdkVersion);
        info.signatures = info.signingInfo.hasPastSigningCertificates()
            ? new android.content.pm.Signature[]{info.signingInfo.getSigningCertificateHistory()[0]}
            : info.signingInfo.getApkContentsSigners();
        info.applicationInfo.sourceDir = file.getPath();
        info.applicationInfo.publicSourceDir = file.getPath();
        return info;
    }

    /** Verifies APK contents, rather than merely reading the signing block's certificate. */
    public static SigningInfo verify(String path, int targetSdk) throws IOException {
        try {
            Class<?> inputType = Class.forName("android.content.pm.parsing.result.ParseInput");
            Class<?> resultType = Class.forName("android.content.pm.parsing.result.ParseResult");
            // Archive verification also runs before application services exist.
            // Use the image's archive-parser policy, which applies target-SDK
            // gates without requiring the platform compatibility Binder service.
            Object input = Class.forName("android.content.pm.parsing.result.ParseTypeImpl")
                .getMethod("forParsingWithoutPlatformCompat").invoke(null);
            Class<?> verifier = Class.forName("android.util.apk.ApkSignatureVerifier");
            int minimum = (Integer) verifier.getMethod("getMinimumSignatureSchemeVersionForTargetSdk", int.class).invoke(null, targetSdk);
            Object result = verifier.getMethod("verify", inputType, String.class, int.class).invoke(null, input, path, minimum);
            if ((Boolean) resultType.getMethod("isError").invoke(result)) {
                int code = (Integer) resultType.getMethod("getErrorCode").invoke(result);
                String message = (String) resultType.getMethod("getErrorMessage").invoke(result);
                Throwable cause = (Throwable) resultType.getMethod("getException").invoke(result);
                throw new IOException("APK verification failed (" + code + "): " + message, cause);
            }
            Object details = resultType.getMethod("getResult").invoke(result);
            return SigningInfo.class.getConstructor(Class.forName("android.content.pm.SigningDetails")).newInstance(details);
        } catch (InvocationTargetException error) {
            throw new IOException("Android APK verifier failed", error.getCause());
        } catch (ReflectiveOperationException error) {
            throw new IOException("Android image APK verifier is unavailable", error);
        }
    }
}
