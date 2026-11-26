package com.trebledot.stone.probe;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import com.trebledot.stone.runtime.ArtArchive;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.util.Properties;

final class ArchiveProbe {
    private static void launchSource(PackageInfo installed, PackageInfo supplied, String source, boolean accepted) throws Exception {
        java.lang.reflect.Method validate = Class.forName("com.trebledot.stone.runtime.ArtPackageStore")
            .getDeclaredMethod("validateLaunchSource",PackageInfo.class,String.class,int.class,long.class);
        validate.setAccessible(true);
        try {
            validate.invoke(null,installed,source,supplied.applicationInfo.targetSdkVersion,supplied.getLongVersionCode());
            if (!accepted) throw new AssertionError("Untrusted or newer launch alias accepted: " + source);
        } catch (java.lang.reflect.InvocationTargetException error) {
            if (accepted || !(error.getCause() instanceof IOException)) throw new AssertionError("Unexpected launch alias verification result: " + source,error.getCause());
        }
    }
    static void run(Context context) throws Exception {
        String root = System.getProperty("stone.probe.archives");
        if (root == null) return;
        Properties expected = new Properties();
        try (FileInputStream input = new FileInputStream(new File(root, "expected.properties"))) { expected.load(input); }
        for (String variant : new String[]{"v1", "v2", "v3", "other", "service"}) {
            PackageInfo info = ArtArchive.inspect(context, new File(root, variant + ".apk").getPath());
            if (!"com.trebledot.stone.install.fixture".equals(info.packageName) || info.getLongVersionCode() != 7
                    || !"7.0".equals(info.versionName) || info.applicationInfo.minSdkVersion != 23
                    || info.applicationInfo.targetSdkVersion != (variant.equals("v1") ? 28 : 32)
                    || info.signingInfo == null || info.signingInfo.hasMultipleSigners() || info.signatures.length != 1
                    || !java.util.Arrays.asList(info.requestedPermissions).contains("android.permission.INTERNET"))
                throw new AssertionError("APK archive metadata: " + variant);
            String identity = variant.equals("other") ? "b" : "a";
            byte[] cert = info.signingInfo.getApkContentsSigners()[0].toByteArray();
            if (!digest(cert).equals(expected.getProperty(identity + ".sha256"))) throw new AssertionError("APK signer identity: " + variant);
            int scheme = info.signingInfo.getSchemeVersion();
            if (scheme != (variant.equals("v1") ? 1 : variant.equals("v3") ? 3 : 2)) throw new AssertionError("APK signature scheme: " + variant + ":" + scheme);
            if (variant.equals("service") && (info.services == null || info.services.length != 1 || info.activities != null))
                throw new AssertionError("service-only archive requires no launcher");
            java.util.List<String> requested = java.util.Arrays.asList(info.requestedPermissions);
            if (!requested.contains("android.permission.ACCESS_COARSE_LOCATION")
                    || requested.contains("android.permission.ACCESS_BACKGROUND_LOCATION") != variant.equals("v1"))
                throw new AssertionError("image permission split target policy: " + variant);
        }
        android.content.pm.Signature signerA = ArtArchive.inspect(context, new File(root, "v2.apk").getPath()).signatures[0];
        android.content.pm.Signature signerB = ArtArchive.inspect(context, new File(root, "other.apk").getPath()).signatures[0];
        PackageInfo rotated = ArtArchive.inspect(context, new File(root, "rotated.apk").getPath());
        if (rotated.signingInfo.hasMultipleSigners() || !rotated.signingInfo.hasPastSigningCertificates()
                || !signerB.equals(rotated.signingInfo.getApkContentsSigners()[0]) || !signerA.equals(rotated.signatures[0])
                || !java.util.Arrays.equals(rotated.signingInfo.getSigningCertificateHistory(), new android.content.pm.Signature[]{signerA, signerB}))
            throw new AssertionError("APK signing certificate rotation history");
        PackageInfo multi = ArtArchive.inspect(context, new File(root, "multi.apk").getPath());
        if (!multi.signingInfo.hasMultipleSigners() || multi.signingInfo.hasPastSigningCertificates()
                || multi.signingInfo.getSigningCertificateHistory() != null || multi.signatures.length != 2
                || !java.util.Arrays.asList(multi.signatures).containsAll(java.util.Arrays.asList(signerA, signerB)))
            throw new AssertionError("APK multiple signer identity");
        for (String invalid : new String[]{"unsigned", "tampered-v1", "tampered-v2", "tampered-v3", "tampered-rotated", "tampered-multi",
                "truncated", "v1-modern", "future", "missing"}) {
            try { ArtArchive.inspect(context, new File(root, invalid + ".apk").getPath()); throw new AssertionError("invalid APK accepted: " + invalid); }
            catch (IOException expectedError) { System.out.println("ART rejected " + invalid + " APK: " + expectedError.getMessage()); }
        }
        PackageManager manager = context.getPackageManager();
        PackageInfo base = ArtArchive.inspect(context,new File(root,"v2.apk").getPath());
        PackageInfo newer = ArtArchive.inspect(context,new File(root,"update.apk").getPath());
        PackageInfo foreign = ArtArchive.inspect(context,new File(root,"other.apk").getPath());
        PackageInfo noData = ArtArchive.inspect(context,new File(root,"rotated-no-data.apk").getPath());
        launchSource(newer,base,base.applicationInfo.sourceDir,true);
        launchSource(base,base,base.applicationInfo.sourceDir,true);
        launchSource(base,newer,newer.applicationInfo.sourceDir,false);
        launchSource(base,foreign,foreign.applicationInfo.sourceDir,false);
        launchSource(rotated,base,base.applicationInfo.sourceDir,true);
        launchSource(noData,base,base.applicationInfo.sourceDir,false);
        launchSource(base,rotated,rotated.applicationInfo.sourceDir,false);
        launchSource(multi,base,base.applicationInfo.sourceDir,false);
        launchSource(multi,multi,multi.applicationInfo.sourceDir,true);
        for (String invalid : new String[]{"unsigned","tampered-v2","tampered-v3","tampered-rotated","tampered-multi"})
            launchSource(base,base,new File(root,invalid+".apk").getPath(),false);
        System.out.println("PASS launch alias verification: same/older trusted APKs, signing rotation and installed-data capability; newer, foreign, multi-signer changes and modified bytes rejected.");
        PackageInfo launched = manager.getPackageInfo(context.getPackageName(),
            PackageManager.GET_SIGNING_CERTIFICATES | PackageManager.GET_SIGNATURES);
        if (launched.signingInfo == null || launched.signatures == null || launched.signatures.length != 1
                || !launched.signatures[0].equals(launched.signingInfo.getApkContentsSigners()[0]))
            throw new AssertionError("launched APK signing identity");
        PackageInfo modern = manager.getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
        PackageInfo legacy = manager.getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNATURES);
        PackageInfo plain = manager.getPackageInfo(context.getPackageName(), 0);
        if (modern.signingInfo == null || modern.signatures != null || legacy.signingInfo != null || legacy.signatures == null
                || plain.signingInfo != null || plain.signatures != null)
            throw new AssertionError("package signing query flags");
        launched.versionName = "mutated";
        launched.signatures[0] = null;
        launched.signingInfo.getApkContentsSigners()[0] = null;
        if (manager.getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNATURES).signatures[0] == null
                || manager.getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES).signingInfo.getApkContentsSigners()[0] == null
                || "mutated".equals(manager.getPackageInfo(context.getPackageName(), 0).versionName))
            throw new AssertionError("package information escaped ownership");
        if (manager.getInstalledPackages(PackageManager.GET_SIGNING_CERTIFICATES).stream()
                .noneMatch(info -> context.getPackageName().equals(info.packageName) && info.signingInfo != null))
            throw new AssertionError("installed package signing metadata");
        if (expected.containsKey("sample.package")) {
            File sample = new File(root, "sample.apk");
            MessageDigest sha256 = MessageDigest.getInstance("SHA-256");
            try (FileInputStream input = new FileInputStream(sample)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) != -1) sha256.update(buffer, 0, count);
            }
            if (!hex(sha256.digest()).equals(expected.getProperty("sample.sha256"))) throw new AssertionError("sample APK identity");
            PackageInfo info = ArtArchive.inspect(context, sample.getPath());
            java.util.List<String> actual = new java.util.ArrayList<>();
            for (android.content.pm.Signature cert : info.signingInfo.getApkContentsSigners()) actual.add(digest(cert.toByteArray()));
            java.util.List<String> wanted = java.util.Arrays.asList(expected.getProperty("sample.certificates").split(","));
            if (!info.packageName.equals(expected.getProperty("sample.package"))
                    || info.getLongVersionCode() != Long.parseLong(expected.getProperty("sample.version"))
                    || actual.size() != wanted.size() || !actual.containsAll(wanted))
                throw new AssertionError("sample APK manifest or signing identity");
            System.out.println("ART fully verified sample APK " + info.packageName + " " + info.getLongVersionCode()
                + " with certificate SHA-256 " + actual);
        }
        System.out.println("ART verified APK v1/v2/v3 contents, signer rotation, multiple signers, service-only metadata and invalid archive rejection.");
    }

    private static String digest(byte[] bytes) throws Exception { return hex(MessageDigest.getInstance("SHA-256").digest(bytes)); }
    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte value : bytes) result.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }
}
