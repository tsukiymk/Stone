set(VCPKG_TARGET_ARCHITECTURE arm64)
# Match Stone's static CRT. General-purpose libraries provide Debug and Release
# builds; the dynamically loaded ANGLE runtime uses its Release DLLs in both.
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
if(PORT STREQUAL "angle")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    set(VCPKG_BUILD_TYPE release)
    # Avoid long virtualenv app-data image paths in Windows workspaces.
    set(ENV{VIRTUALENV_SEEDER} pip)
endif()
