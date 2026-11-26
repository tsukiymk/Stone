package com.trebledot.stone.runtime;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Runs the runtime JAR's preflight on synthetic APK layouts without an Android image. */
final class ArtPackageCompatibilityTests {
    private interface Check { void run() throws Exception; }

    private static void reject(Check operation, boolean incompatible, String detail) throws Exception {
        try { operation.run(); throw new AssertionError("Accepted incompatible/invalid input: " + detail); }
        catch (IOException expected) {
            if ((expected instanceof ArtPackageCompatibility.Incompatible) != incompatible
                    || !expected.getMessage().contains(detail))
                throw new AssertionError("Incorrect failure classification or missing reason", expected);
        }
    }

    private static void archive(File file, String... entries) throws IOException {
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(file))) {
            for (String name : entries) {
                zip.putNextEntry(new ZipEntry(name));
                if (!name.endsWith("/")) zip.write(new byte[]{1, 2, 3, 4});
                zip.closeEntry();
            }
        }
    }

    private static void declaredSizes(File file, int size) throws IOException {
        byte[] bytes = Files.readAllBytes(file.toPath());
        // Change the central-directory uncompressed sizes without allocating
        // hundreds of MB. Preflight must reject these before extraction.
        for (int index = 0; index + 46 <= bytes.length; index++) {
            if (bytes[index] != 0x50 || bytes[index + 1] != 0x4b || bytes[index + 2] != 1 || bytes[index + 3] != 2) continue;
            for (int part = 0; part < 4; part++) bytes[index + 24 + part] = (byte) (size >>> (part * 8));
        }
        Files.write(file.toPath(), bytes);
    }

    public static void main(String[] arguments) throws Exception {
        File root = new File(arguments[0]);
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create test directory");
        File apk = new File(root, "candidate.apk");
        try {
            ArtPackageCompatibility.checkSdk(23, 37);
            ArtPackageCompatibility.checkSdk(37, 37);
            reject(() -> ArtPackageCompatibility.checkSdk(38, 37), true, "API 38; selected image provides API 37");
            try { ArtPackageCompatibility.checkSdk(38, 37); throw new AssertionError("SDK accepted"); }
            catch (ArtPackageCompatibility.Incompatible expected) {
                if (!"android-api".equals(expected.reason) || !"38".equals(expected.required) || !"37".equals(expected.available)
                        || ArtPackageCompatibility.failure(new IOException("wrapped install failure", expected)) != expected)
                    throw new AssertionError("Structured SDK failure or wrapped-cause identity lost");
            }
            if (ArtPackageCompatibility.failure(new IOException("ordinary failure")) != null)
                throw new AssertionError("Ordinary failure mislabeled as incompatibility");
            if (!"arm64-v8a".equals(ArtPackageCompatibility.imageAbi(new String[]{"arm64-v8a", "x86_64"})))
                throw new AssertionError("Did not select the active VM ABI");
            reject(() -> ArtPackageCompatibility.imageAbi(new String[0]), true, "64-bit");
            reject(() -> ArtPackageCompatibility.imageAbi(null), true, "64-bit");
            reject(() -> ArtPackageCompatibility.imageAbi(new String[]{"armeabi-v7a"}), true, "64-bit");
            reject(() -> ArtPackageCompatibility.imageAbi(new String[]{"unknown", "x86_64"}), true, "64-bit");

            archive(apk, "classes.dex", "assets/libplugin.so", "lib/x86_64/", "lib/arm64-v8a/directory.so/");
            ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64");
            ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "arm64-v8a");
            archive(apk, "lib/x86_64/libprobe.so", "lib/armeabi-v7a/libprobe.so");
            ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64");
            reject(() -> ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "arm64-v8a"), true, "requires arm64-v8a");
            archive(apk, "lib/armeabi-v7a/libprobe.so", "lib/x86/libprobe.so");
            reject(() -> ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64"), true, "armeabi-v7a, x86");
            archive(apk, "lib/arm64-v8a/libprobe.so");
            reject(() -> ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64"), true, "requires x86_64");
            archive(apk, "lib/x86_64/libprobe.so", "lib/arm64-v8a/libprobe.so");
            ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64");
            ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "arm64-v8a");
            for (String name : new String[]{"../escape.so", "nested/libprobe.so", "bad\\name.so", "bad:name.so", ".hidden.so"}) {
                archive(apk, "lib/x86_64/" + name);
                reject(() -> ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64"), false, "Invalid APK native library");
            }
            archive(apk, "lib/x86_64/liba.so", "lib/x86_64/libb.so");
            // ZIP writers disallow duplicate names. Mutate both same-length
            // directory/local names to exercise the reader's duplicate defense.
            String bytes = new String(Files.readAllBytes(apk.toPath()), StandardCharsets.ISO_8859_1);
            Files.write(apk.toPath(), bytes.replace("libb.so", "liba.so").getBytes(StandardCharsets.ISO_8859_1));
            reject(() -> ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64"), false, "Duplicate APK native library");
            archive(apk, "lib/x86_64/libprobe.so");
            declaredSizes(apk, 257 * 1024 * 1024);
            reject(() -> ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64"), false, "size limit");
            archive(apk, "lib/x86_64/liba.so", "lib/x86_64/libb.so", "lib/x86_64/libc.so");
            declaredSizes(apk, 200 * 1024 * 1024);
            reject(() -> ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64"), false, "size limit");
            Files.write(apk.toPath(), new byte[]{1, 2, 3});
            reject(() -> ArtPackageCompatibility.checkNativeLibraries(apk.getPath(), "x86_64"), false, "zip");
        } finally { Files.deleteIfExists(apk.toPath()); }
        System.out.println("PASS APK compatibility: SDK, Java-only, active image ABI, 32-bit/cross-ABI rejection, universal APKs and malformed native layouts");
    }
}
