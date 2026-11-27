# Files, storage, and documents

The system image provides Android resources read-only. Application code, private data, shared volumes, and caches use the Windows file backend under the selected data root. Android paths are not aliases for arbitrary host paths; access must pass mount and application identity checks.

## Shared volumes and application directories

The primary volume's Android path is `/storage/emulated/0`, backed by `<data-dir>/storage/emulated/0` on the host. `/sdcard`, `/storage/self/primary`, and `/data/media/0` refer to the same volume.

When initializing shared storage for an existing data root, Stone checks `<data-dir>/sdcard`, then `<data-dir>/data/media/0`, and moves the first existing directory to the primary volume path if that destination does not exist. An existing destination is neither overwritten nor merged with old data.

Volume initialization creates public directories. `Android/data/<package>` and `Android/obb/<package>` are created on demand when requested by the application. Installation, launch, and all-files access grants do not create app-specific external directories in advance.

Applications can access their own app-specific external directories. Access elsewhere on the shared volume requires a manifest declaration of MANAGE_EXTERNAL_STORAGE and a grant. Even with that grant, other packages' app-specific subtrees remain inaccessible. Grants bind to UID and the volume/file identity of the private directory; recreating the directory does not preserve the grant. Case differences, links, and aliases cannot bypass these boundaries.

## Windows file bridging

File descriptors, metadata, path normalization, links, and directory access preserve Android numeric and error semantics. Supported symlink/readlink operations use the Windows file backend. Unconnected operations fail explicitly rather than returning Windows errno values as Android errno.

Java SharedMemory uses the image JNI and Stone's memfd backend for creation, mapping, size queries, and write seals. Current-process `/proc/self/fd/<fd>` and `/proc/<pid>/fd/<fd>` links identify registered memory files with their memfd names; duplicated descriptors retain that identity and closed descriptors no longer resolve. This does not expose arbitrary Windows paths or another process's descriptors.

`/proc/self/cmdline` and `/proc/<current-pid>/cmdline` support read-only absolute-path opens, including `openat` and `fopen`. Each open reads an independent, NUL-terminated snapshot of the Android process name, set before Application initialization and including a private-process suffix when present. Before application identity is available, the name is `Stone`. These files do not expose the Windows command line, other processes, or general procfs directory traversal.

inotify uses Windows asynchronous directory notifications. It supports nonrecursive creation, deletion, modification, and same-directory rename events, with queues, masks, and poll/epoll. Permission revocation clears pending data. Ordinary file watches, complete open/read events, and matching moves across directories remain limited.

## Windows file dialogs

ACTION_CREATE_DOCUMENT opens a native save dialog on Client's background STA thread while the main Looper continues running. Confirmation returns a content URI usable only by the current application. Cancellation returns a cancelled result without creating a grant. Files are read, written, truncated, or appended in the requested mode only when the application actually opens the URI.

Native handles remain in Client, and URIs do not expose host paths. Each open has an independent position. Revocation closes retained handles, while already opened streams may finish. Service neither selects files on the user's behalf nor holds ART objects from another process.

ACTION_OPEN_DOCUMENT_TREE provides a user-selected read-only subtree, metadata, and reads. Persistent grants bind to the application, data root, and directory identity. Single-file persistent grants, document modification, and cross-package sharing are not fully connected.

## SQLite

Windows x64 uses image `/system/lib64/libsqlite.so`; Java SQLite also uses the image implementation. Stone VFS provides files, rollback journals, and WAL shared indexes, with mappings and cross-process locks managed by file identity.

For the selected SQLite version, Stone VFS adds mutual exclusion between writes and checkpoints while retaining ordinary read snapshots. Backups must still follow SQLite transaction and WAL rules; do not directly copy a main file being written. Windows ARM64 has no Android ELF SQLite backend.

## Limits

The implementation does not provide full FUSE, MediaStore, removable/adoptable storage, or Android scoped storage. Read-only system mounts, shared-volume grants, and document trees are separate access boundaries and cannot substitute for one another.
