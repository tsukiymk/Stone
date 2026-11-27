# Package management, installation, and updates

Stone uses the image PackageManager, manifest parser, and signature verifier to handle APKs. Installation confirmation is presented by the native settings window or a fallback Android page. Signatures, session state, and package publication transactions remain owned by ART.

## Installation compatibility checks

Before requesting installation confirmation or publishing an automatic update, Stone checks the owned candidate copy against the selected image's Android API level and actual ART ABI. APKs requiring a newer API or containing native libraries only for other ABIs are rejected with a reason identifying the required and available API/ABI. Only the active 64-bit image ABI (`x86_64` or `arm64-v8a`) is eligible; 32-bit native libraries and cross-ABI execution within that VM are unsupported. Java-only APKs need no native ABI match, and universal APKs may include other ABIs alongside the active one.

Installation and launch share native-library layout checks, including duplicate names, invalid paths, and extraction size limits. Shared-UID and split APK installation remain unsupported. Compatibility failures return `PackageInstaller.STATUS_FAILURE_INCOMPATIBLE`; malformed APK/native layouts return `STATUS_FAILURE_INVALID`. Settings displays the preparation error without enabling installation. Failed preparation removes the staged copy and preserves the installed version and application data.

These checks detect structural requirements, not full application compatibility. They do not execute application code, resolve every native dependency or symbol, or establish support for all requested Android APIs, hardware features, or services. Successful installation still requires separate application behavior testing.

## Sessions and confirmation

PackageInstaller sessions support staged reads and writes, resuming writes at an offset, fsync, progress, and cancellation. Parameters, labels, and icons are stored by value. Uncommitted sessions do not become installed packages. Staging uses `data/system/stone-package-installer/`, with metadata updated through temporary files and atomic replacement.

Commit first prepares a read-only candidate copy, then obtains a decision through the same controlled confirmation flow. Session nonce, caller, package name, and candidate identity are rechecked at commit. Cancellation, failure, or storage errors do not report success. Installation preapproval only authorizes continued processing after download; the actual APK still requires full signature verification and must match the preapproved package name and application name.

## Signatures and publication

The image verifier checks complete APK contents and supported v1/v2/v3 signatures. Reading certificates is not equivalent to verifying contents. Updates check versions, current signatures, multiple-signer sets, and data inheritance capabilities in image SigningDetails, rejecting downgrades and unauthorized certificate changes.

Package code is published to `/data/app/stone/<uuid>/base.apk`, with the index under `/data/system/stone-packages/`. Cross-process file locks serialize commits. Atomic indexes and commit receipts support fault recovery. Updates preserve UID, first installation time, and private data. Query results do not share mutable cached objects.

Both `getInstallerPackageName` and `getInstallSourceInfo` read the current install-source registry. For a known package without a recorded installer, including an unregistered APK launched directly, the installer name is `null`. `getInstallerPackageName` throws `IllegalArgumentException` when the queried package is neither registered nor the current application.

## Updates without confirmation and ownership

`USER_ACTION_NOT_REQUIRED` requests installation without confirmation, but still requires actual manifest permissions, the platform's minimum target API level, and existing install-source/update-ownership rules. Signature conflicts, downgrades, mandatory confirmation, or insufficient eligibility still fail or require confirmation.

Ownership is established only at initial installation, then retained or cleared according to the update source and confirmation through other sources. Uninstalling an installer clears references to that source; reinstallation under the same name cannot inherit update eligibility. Eligibility is evaluated against current state under the package publication lock, not an old preparation snapshot.

## Launch entry points and package changes

Application entry points resolve against current component enabled state, launcher aliases, and registered code. Permitted data inheritance can route an old APK entry point to the currently registered version. Inputs with different signatures or outside the registered version must not silently replace installed code.

Client draws icons in the background. Service manages Start menu entries, stable taskbar identity, and package-name protocol entry points. Shortcuts use application display names; name conflicts do not overwrite another application's entry.

Package catalog refresh also renders application icons through the selected image's resources into a small atomic PNG cache for Settings. Installation confirmation uses the prepared candidate's icon; session-supplied presentation metadata cannot replace the verified APK icon. Missing resources do not block package operations.

Applications sharing a data root receive installation, update, and removal events through background observers. Package index file identity detects atomic replacement. Event delivery, interface caches, and actual package queries remain separate layers.

## Removal and limits

Removal confirmation binds to the target code identity. After publishing deletion state, cleanup removes code, data, launch profiles, jobs, notifications, and Shell entries. Index failure preserves the original package. Persistent deletion state supports recovery of unfinished cleanup. Cancellation does not delete data.

Multi-package parent sessions, staged/DataLoader installation, incremental/v4 installation, complete code reclamation, PackageInstaller.uninstall, and all coordination for uninstalling running applications are not fully connected. Removing a storage registration does not replace complete uninstall semantics.
