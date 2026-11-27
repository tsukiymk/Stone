# Stone documentation

Stone is an Android compatibility layer for Windows. This documentation covers usage, process and source architecture, runtimes, feature limits, and build dependencies. See the root [README.md](../README.md) for a project overview and [AGENTS.md](../AGENTS.md) for repository contribution guidelines.

## Documentation standards

- Organize content by user task or technical responsibility. Describe current behavior, prerequisites, configuration, failure handling, and supported limits.
- Update the relevant feature page when behavior changes. Keep chronological progress reports, completed-task lists, debugging transcripts, and one-off build or test output in issues or pull requests.
- Include implementation details when they explain a component boundary, contract, or maintenance procedure. Omit isolated fix details and descriptions of abandoned implementations.
- Keep migration guidance only when it identifies an affected path or setting and explains the current behavior or required action. Preserve third-party source identities, licenses, and adaptation notes.
- Keep each procedure or contract in one primary location and link to it from related pages. Remove repeated claims that add no feature-specific information.
- Check commands, paths, options, and local links against the current source and CMake rules. State validation scope and limitations; distinguish implemented behavior from planned or unverified support.

## Usage and development

- [Using Stone](usage.md): launch arguments, data roots, and execution options
- [Building from source](development/build.md): tools, artifacts, and publishing
- [Logging and diagnostics](development/diagnostics.md): application logs, software frames, and troubleshooting

## Architecture

- [Architecture and source responsibilities](architecture/overview.md)
- [Native compatibility layers](architecture/native-layers.md)

## Runtimes

- [ART and system images](runtime/art.md)
- [Experimental APK AOT](runtime/art-aot.md)
- [ARM64 translation and native block caching](runtime/arm64-translation-cache.md)

## Android features

- [Application lifecycle and Activity routing](android/lifecycle.md)
- [Package management, installation, and updates](android/packages.md)
- [Files, storage, and documents](android/storage.md)
- [Networking and local communication](android/network.md)
- [Graphics, windows, input, and configuration](android/graphics-input.md)
- [Audio and media](android/audio.md)
- [Power, jobs, and system services](android/system-services.md)

## Windows integration

- [Desktop integration](windows/integration.md): notifications, clipboard, sharing, and browsers
- [Stone settings](windows/settings.md): system images, application management, permissions, and native confirmation
- [Windows packages](windows/packaging.md): MSI, MSIX, MSIXBundle, building, and signing

## Third-party components

- [Third-party notices](third-party/THIRD_PARTY_NOTICES.txt)
- [Bionic error description sources](third-party/bionic.md)
- [XZ Embedded](third-party/xz-embedded.md)
- [Zydis and Zycore](third-party/zydis.md)
- [Windows ANGLE runtime](third-party/angle.md)
