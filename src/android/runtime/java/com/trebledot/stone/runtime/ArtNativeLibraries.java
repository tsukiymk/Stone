package com.trebledot.stone.runtime;

import android.content.pm.ApplicationInfo;
import android.os.Build;
import java.io.*;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.*;
import java.util.zip.*;

/** Prepares the selected APK's libraries before constructing its class loader. */
final class ArtNativeLibraries {
    private ArtNativeLibraries() {}

    static void prepare(ApplicationInfo info) throws IOException {
        String abi = ArtPackageCompatibility.imageAbi(Build.SUPPORTED_64_BIT_ABIS);
        String identity = digest(new File(info.sourceDir));
        File directory = new File(info.dataDir, "code_cache/stone-native/" + identity + "/" + abi);
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot prepare APK native libraries");
        boolean hasLibraries;
        try (ZipFile zip = new ZipFile(info.sourceDir)) {
            List<ZipEntry> entries = ArtPackageCompatibility.nativeLibraries(zip, abi);
            hasLibraries = !entries.isEmpty();
            for (ZipEntry entry : entries) {
                String path = entry.getName();
                String name = path.substring(path.lastIndexOf('/') + 1);
                long size = entry.getSize();
                File target = new File(directory, name);
                // Verify warm files too; a writable or stale copy must never be loaded.
                String expected;
                try (InputStream input = zip.getInputStream(entry)) { expected = digest(input); }
                if (target.isFile() && !target.canWrite() && expected.equals(digest(target))) continue;
                File temporary = File.createTempFile("native-", ".tmp", directory);
                try {
                    try (InputStream input = zip.getInputStream(entry); OutputStream output = new FileOutputStream(temporary)) {
                        byte[] buffer = new byte[65536];
                        long written = 0;
                        for (int count; (count = input.read(buffer)) != -1;) {
                            if ((written += count) > size) throw new IOException("Invalid APK native library size");
                            output.write(buffer, 0, count);
                        }
                        if (written != size) throw new IOException("Truncated APK native library");
                    }
                    if (!expected.equals(digest(temporary)) || !temporary.setReadOnly())
                        throw new IOException("Cannot seal APK native library");
                    if (target.exists() && (!target.setWritable(true) || !target.delete())) throw new IOException("Cannot replace APK native library");
                    if (!temporary.renameTo(target)) throw new IOException("Cannot publish APK native library");
                } finally { temporary.delete(); }
            }
        }
        info.nativeLibraryDir = hasLibraries ? directory.getPath() : null;
    }

    private static String digest(File file) throws IOException {
        try (InputStream input = new FileInputStream(file)) { return digest(input); }
    }

    static String prepareImageProvider(ApplicationInfo info,String data,String library) throws IOException {
        ApplicationInfo extraction=new ApplicationInfo(info); extraction.dataDir=data;
        prepare(extraction);
        if(extraction.nativeLibraryDir!=null) return extraction.nativeLibraryDir;
        File source=imageLibrary(info,library);
        if(source==null) throw new IOException("Image provider native library is absent");
        String expected=digest(source);
        File directory=new File(data,"code_cache/stone-webview/"+digest(new File(info.sourceDir))+"-"+expected);
        if(!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot prepare WebView native cache");
        File target=new File(directory,library);
        if(!target.isFile() || target.canWrite() || !expected.equals(digest(target))) {
            File temporary=File.createTempFile("webview-",".tmp",directory);
            try {
                try(InputStream input=new FileInputStream(source); OutputStream output=new FileOutputStream(temporary)) {
                    byte[] buffer=new byte[65536]; long total=0;
                    for(int count;(count=input.read(buffer))!=-1;) {
                        if((total+=count)>256L*1024*1024) throw new IOException("WebView native library exceeds size limit");
                        output.write(buffer,0,count);
                    }
                }
                if(!expected.equals(digest(temporary)) || !temporary.setReadOnly()) throw new IOException("Cannot seal WebView native library");
                if(target.exists() && (!target.setWritable(true) || !target.delete())) throw new IOException("Cannot replace WebView native cache");
                if(!temporary.renameTo(target)) throw new IOException("Cannot publish WebView native library");
            } finally { temporary.delete(); }
        }
        return directory.getPath();
    }

    static File imageLibrary(ApplicationInfo info,String library) throws IOException {
        String isa="x86_64".equals(Build.SUPPORTED_64_BIT_ABIS[0])?"x86_64":"arm64";
        File parent=new File(info.sourceDir).getCanonicalFile().getParentFile();
        File file=new File(parent,"lib/"+isa+"/"+library).getCanonicalFile();
        return file.getPath().startsWith(parent.getPath()+"/") && file.isFile()?file:null;
    }
    private static String digest(InputStream input) throws IOException {
        try {
            MessageDigest hash = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[65536];
            for (int count; (count = input.read(buffer)) != -1;) hash.update(buffer, 0, count);
            StringBuilder result = new StringBuilder(64);
            for (byte value : hash.digest()) result.append(Character.forDigit((value & 255) >>> 4, 16)).append(Character.forDigit(value & 15, 16));
            return result.toString();
        } catch (NoSuchAlgorithmException error) { throw new IOException("SHA-256 is unavailable", error); }
    }
}
