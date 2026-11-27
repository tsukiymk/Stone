# Windows ANGLE runtime

Stone uses ANGLE for its Windows EGL/GLES implementation. See the [build instructions](../development/build.md#building-the-graphics-runtime) for dependency restoration and runtime deployment, and [graphics documentation](../android/graphics-input.md#software-drawing-and-bitmaps) for backend selection and DLL loading.

## Sources

Stone uses the official vcpkg `angle` port (`chromium_7258`, port revision 2), based on ANGLE source commit [`d9fc4a372074b1079c193c422fc4a180e79b6636`](https://github.com/google/angle/tree/d9fc4a372074b1079c193c422fc4a180e79b6636). The dependency baseline is pinned to vcpkg commit [`e6d62cc9ce8f8d9687df14521118f2ec3c10ee6a`](https://github.com/microsoft/vcpkg/tree/e6d62cc9ce8f8d9687df14521118f2ec3c10ee6a). The root [`vcpkg.json`](../../vcpkg.json) records the baseline, ANGLE version override, and overlay configuration. The package's `vcpkg.spdx.json` records upstream source provenance and is distributed unchanged with the runtime licenses.

## Build adaptation

The pinned port provides the CMake build adaptation and patches, converts upstream GN source lists using WebKit's converter, and verifies source archive hashes. Its Windows backends include D3D11/WARP, D3D9, and desktop OpenGL. Stone uses the port without additional patches or post-processing. Stone's [triplets](../../dep/vcpkg-overlay-triplets/) select DLL linkage for ANGLE and static linkage for the CRT and implementation dependencies.

The [OpenGL registry overlay](../../dep/vcpkg-overlay-ports/opengl-registry/portfile.cmake) fetches the official port's source commit [`e8f7cd0e35ac8d6f5667a021ff83d04b1fec41ef`](https://github.com/KhronosGroup/OpenGL-Registry/tree/e8f7cd0e35ac8d6f5667a021ff83d04b1fec41ef) using a Git partial checkout of `api` and `xml`, avoiding the full specification archive. It preserves the official port's installed headers, XML, pkg-config file, and copyright notice.

Changes to source pins or triplets require rebuilding and reviewing dependency licenses and runtime behavior.

## Licenses and distributed notices

The runtime includes these files from the vcpkg target installation:

| Installed source | Distributed path |
| --- | --- |
| `share/angle/copyright` | `licenses/angle/LICENSE` |
| `share/zlib/copyright` | `licenses/angle/zlib.txt` |
| `share/egl-registry/copyright` | `licenses/angle/egl-registry.txt` |
| `share/opengl-registry/copyright` | `licenses/angle/opengl-registry.txt` |
| `share/angle/vcpkg.spdx.json` | `licenses/angle/vcpkg.spdx.json` |

Installed headers retain their individual notices. The bundled `d3dcompiler_47.dll` comes from the selected Windows SDK redistributable directory and is distributed under the [Windows SDK license](https://aka.ms/WinSDKLicenseURL). See the [third-party notices](THIRD_PARTY_NOTICES.txt) for the component inventory.
