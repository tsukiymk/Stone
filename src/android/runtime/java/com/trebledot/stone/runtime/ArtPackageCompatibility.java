package com.trebledot.stone.runtime;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Structural installation checks; passing these checks does not prove application functionality. */
final class ArtPackageCompatibility {
    private ArtPackageCompatibility() {}

    static final class Incompatible extends IOException {
        final String reason, required, available;
        Incompatible(String reason, String required, String available, String message) {
            super(message); this.reason = reason; this.required = required; this.available = available;
        }
    }

    static Incompatible failure(Throwable error) {
        for (int depth = 0; error != null && depth < 16; depth++, error = error.getCause())
            if (error instanceof Incompatible) return (Incompatible) error;
        return null;
    }

    static void checkSdk(int minimum, int available) throws Incompatible {
        if (minimum > available)
            throw new Incompatible("android-api", Integer.toString(minimum), Integer.toString(available),
                "APK requires Android API " + minimum + "; selected image provides API " + available);
    }

    static String imageAbi(String[] abis) throws Incompatible {
        if (abis == null || abis.length == 0 || !("x86_64".equals(abis[0]) || "arm64-v8a".equals(abis[0])))
            throw new Incompatible("image-abi", "", "", "Selected image has no supported 64-bit application ABI");
        // The ART host advertises its actual ELF architecture. Never select a
        // secondary or 32-bit ABI that the active VM cannot execute.
        return abis[0];
    }

    static void checkNativeLibraries(String path, String abi) throws IOException {
        try (ZipFile zip = new ZipFile(path)) { nativeLibraries(zip, abi); }
    }

    /** Shared with extraction so installation and launch accept the same native layout. */
    static List<ZipEntry> nativeLibraries(ZipFile zip, String abi) throws IOException {
        String prefix = "lib/" + abi + "/";
        List<ZipEntry> selected = new ArrayList<>();
        Set<String> names = new HashSet<>();
        Set<String> available = new TreeSet<>();
        long total = 0;
        Enumeration<? extends ZipEntry> entries = zip.entries();
        while (entries.hasMoreElements()) {
            ZipEntry entry = entries.nextElement();
            String path = entry.getName();
            if (entry.isDirectory() || !path.startsWith("lib/") || !path.endsWith(".so")) continue;
            int separator = path.indexOf('/', 4);
            if (separator <= 4) throw new IOException("Invalid APK native library: " + path);
            available.add(path.substring(4, separator));
            if (!path.startsWith(prefix)) continue;
            String name = path.substring(prefix.length());
            if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0
                    || name.indexOf(':') >= 0 || name.startsWith("."))
                throw new IOException("Invalid APK native library: " + path);
            if (!names.add(name)) throw new IOException("Duplicate APK native library: " + path);
            long size = entry.getSize();
            if (size < 0 || size > 256L * 1024 * 1024 || (total += size) > 512L * 1024 * 1024)
                throw new IOException("APK native libraries exceed size limit");
            selected.add(entry);
        }
        if (!available.isEmpty() && selected.isEmpty())
            throw new Incompatible("native-abi", String.join(", ", available), abi, "APK native ABIs: " + String.join(", ", available)
                + "; selected image requires " + abi + " (32-bit and cross-ABI native libraries are unsupported)");
        return selected;
    }
}
