package com.trebledot.stone.probe;

import android.content.Context;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.nio.file.*;
import java.util.*;

/** Image libcore file and directory link semantics, using only an owned cache directory. */
final class SymbolicLinkProbe {
    private interface Action { void run() throws Exception; }
    private static void errno(int code, Action action) throws Exception {
        try { action.run(); throw new AssertionError("Expected errno "+code); }
        catch (ErrnoException error) { if (error.errno != code) throw error; }
    }
    static void verify(Context context) throws Exception {
        Path root = Files.createTempDirectory(context.getCacheDir().toPath(),"stone-links-");
        List<Path> links = new ArrayList<>();
        try {
            Path source = root.resolve("目标.txt"), link = root.resolve("alias");
            byte[] bytes = "Stone 链接数据".getBytes(java.nio.charset.StandardCharsets.UTF_8);
            Files.write(source,bytes); Os.symlink(source.getFileName().toString(),link.toString()); links.add(link);
            if (!Os.readlink(link.toString()).equals(source.getFileName().toString()) || !Files.isSymbolicLink(link)
                    || !OsConstants.S_ISLNK(Os.lstat(link.toString()).st_mode) || Os.lstat(link.toString()).st_size != source.getFileName().toString().getBytes(java.nio.charset.StandardCharsets.UTF_8).length
                    || !OsConstants.S_ISREG(Os.stat(link.toString()).st_mode) || !Arrays.equals(Files.readAllBytes(link),bytes)
                    || !link.toFile().getCanonicalFile().equals(source.toFile().getCanonicalFile()))
                throw new AssertionError("Relative Unicode file link metadata/read/canonicalization failed");
            errno(OsConstants.EEXIST,() -> Os.symlink("other",link.toString()));
            errno(OsConstants.ELOOP,() -> Os.open(link.toString(),OsConstants.O_RDONLY|OsConstants.O_NOFOLLOW,0));
            errno(OsConstants.EEXIST,() -> Os.open(link.toString(),OsConstants.O_WRONLY|OsConstants.O_CREAT|OsConstants.O_EXCL,0600));
            Path folder = Files.createDirectory(root.resolve("目录")), folderLink = root.resolve("directory-alias");
            Files.write(folder.resolve("value.bin"),bytes);
            Os.symlink(folder.toString(),folderLink.toString()); links.add(folderLink);
            if (!Files.isDirectory(folderLink) || Files.isDirectory(folderLink,LinkOption.NOFOLLOW_LINKS)
                    || !Arrays.equals(Files.readAllBytes(folderLink.resolve("value.bin")),bytes)) throw new AssertionError("Directory prefix link failed");
            Files.write(folderLink.resolve("created.bin"),bytes);
            if (!Arrays.equals(Files.readAllBytes(folder.resolve("created.bin")),bytes)) throw new AssertionError("Write through directory link failed");
            Set<String> entries = new HashSet<>();
            try (DirectoryStream<Path> stream = Files.newDirectoryStream(folderLink)) { for (Path item : stream) entries.add(item.getFileName().toString()); }
            if (!entries.equals(new HashSet<>(Arrays.asList("value.bin","created.bin")))) throw new AssertionError("Directory enumeration through link failed");
            Path missing = root.resolve("later.txt"), dangling = root.resolve("dangling");
            Os.symlink(missing.getFileName().toString(),dangling.toString()); links.add(dangling);
            if (Files.exists(dangling) || !Files.isSymbolicLink(dangling)) throw new AssertionError("Dangling link state failed");
            errno(OsConstants.ENOENT,() -> Os.stat(dangling.toString()));
            errno(OsConstants.EEXIST,() -> Os.mkdir(dangling.toString(),0700));
            Files.write(missing,bytes);
            if (!Arrays.equals(Files.readAllBytes(dangling),bytes)) throw new AssertionError("Dangling link did not follow a later target");
            Path child = Files.createDirectory(root.resolve("relocated")), moved = child.resolve("alias");
            Files.move(link,moved); links.add(moved);
            if (Files.exists(moved) || !Os.readlink(moved.toString()).equals(source.getFileName().toString()) || !Files.exists(source))
                throw new AssertionError("Relative link move retained its former parent");
            Files.write(child.resolve(source.getFileName()),new byte[]{42});
            if (!Arrays.equals(Files.readAllBytes(moved),new byte[]{42})) throw new AssertionError("Moved relative link target failed");
            Path cycleA = root.resolve("cycle-a"), cycleB = root.resolve("cycle-b");
            Os.symlink("cycle-b",cycleA.toString()); links.add(cycleA); Os.symlink("cycle-a",cycleB.toString()); links.add(cycleB);
            errno(OsConstants.ELOOP,() -> Os.stat(cycleA.toString()));
            if (!OsConstants.S_ISLNK(Os.lstat(cycleA.toString()).st_mode)) throw new AssertionError("Cycle lstat followed its target");
            Path image = Paths.get("/system/build.prop"), imageLink = root.resolve("system-alias");
            byte[] original = Files.readAllBytes(image);
            Os.symlink(image.toString(),imageLink.toString()); links.add(imageLink);
            if (!Arrays.equals(Files.readAllBytes(imageLink),original)) throw new AssertionError("Read-only image link did not resolve");
            errno(OsConstants.EROFS,() -> Os.open(imageLink.toString(),OsConstants.O_WRONLY|OsConstants.O_TRUNC,0));
            if (!Arrays.equals(Files.readAllBytes(image),original)) throw new AssertionError("Image link write changed the image");
            Os.remove(folderLink.toString());
            if (!Files.exists(folder.resolve("value.bin")) || Files.exists(folderLink,LinkOption.NOFOLLOW_LINKS)) throw new AssertionError("Unlink followed directory link");
            System.out.println("PASS image symbolic links: relative Unicode and absolute file/directory links, metadata, canonicalization, reads/writes/listing, move, dangling targets, exclusive/no-follow opens, cycles and read-only image protection.");
        } finally {
            for (Path link : links) try { Os.remove(link.toString()); } catch (ErrnoException error) { if (error.errno != OsConstants.ENOENT) throw error; }
            Files.walkFileTree(root,new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path file,java.nio.file.attribute.BasicFileAttributes attributes) throws java.io.IOException { Files.delete(file); return FileVisitResult.CONTINUE; }
                @Override public FileVisitResult postVisitDirectory(Path dir,java.io.IOException error) throws java.io.IOException { if (error != null) throw error; Files.delete(dir); return FileVisitResult.CONTINUE; }
            });
        }
    }
}
