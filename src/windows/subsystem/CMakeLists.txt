set(ZYDIS_FEATURE_FORMATTER OFF CACHE BOOL "Build the Zydis instruction formatter")
add_subdirectory("${PROJECT_SOURCE_DIR}/oss/zydis" "${CMAKE_BINARY_DIR}/oss/zydis" EXCLUDE_FROM_ALL)
add_subdirectory("${PROJECT_SOURCE_DIR}/oss/xz-embedded" "${CMAKE_BINARY_DIR}/oss/xz-embedded" EXCLUDE_FROM_ALL)
target_link_libraries(stone-host PUBLIC Zydis stone-xz)

target_include_directories(stone-host PRIVATE "${PROJECT_SOURCE_DIR}/oss/bionic")

# Instruction dispatch, cache admission and opt-in counters run in real ART/JIT in
# Debug. Keep PDBs, debug CRT and explicit guest bounds checks in these sources.
# MSVC /RTC is incompatible with /O2; all other Debug sources retain /RTC.
if(MSVC AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/cpu/arm64/Arm64Initializer.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/cpu/arm64/Arm64ExecutionProfile.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/cpu/arm64/Arm64BlockJit.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/cpu/arm64/cache/Arm64BlockCache.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/cpu/arm64/cache/Arm64CacheFormat.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/Arm64CacheStorage.cpp"
        TARGET_DIRECTORY stone-host APPEND PROPERTY VS_SETTINGS
        "$<$<CONFIG:Debug>:Optimization=MaxSpeed>"
        "$<$<CONFIG:Debug>:InlineFunctionExpansion=AnySuitable>"
        "$<$<CONFIG:Debug>:BasicRuntimeChecks=Default>")
endif()

target_sources(stone-host PRIVATE
    abi/AndroidAbi.asm
    signals/AndroidSignalContext.cpp
    signals/AndroidSignalContext.h
    abi/TypedAndroidImport.cpp
    abi/TypedAndroidImport.h
    hle/SystemLibraryRegistry.cpp
    hle/SystemLibraryRegistry.h
    loader/AndroidSymbolResolver.cpp
    loader/AndroidSymbolResolver.h
    abi/LibraryIntegration.cpp
    loader/AndroidLibraryState.h
    abi/AndroidCallBridge.cpp
    abi/LibraryJni.cpp
    abi/Arm64LibraryBridge.cpp
    signals/WindowsAndroidExceptions.cpp
    loader/AndroidLibraries.cpp
    abi/AndroidLibraryImports.h
    platform/windows/GuestPaths.cpp
    platform/windows/GuestPaths.h
    platform/windows/GuestAllocation.cpp
    platform/windows/GuestAllocation.h
    memory/GuestHeap.cpp
    memory/GuestHeap.h
    memory/GuestMemory.h
    signals/Arm64SignalContext.h
    abi/Arm64ExecutionBridge.h
    abi/Arm64JniSignature.cpp
    abi/Arm64JniSignature.h
    abi/Arm64GuestJniTables.cpp
    abi/Arm64GuestJniTables.h
    filesystem/AndroidSystemFiles.cpp
    filesystem/AndroidSystemFiles.h
    filesystem/AndroidFileObserver.cpp
    filesystem/AndroidFileObserver.h
    loader/AndroidSystemElf.h
    cpu/arm64/Arm64Initializer.cpp
    cpu/arm64/Arm64Initializer.h
    cpu/arm64/Arm64ExecutionProfile.cpp
    cpu/arm64/Arm64ExecutionProfile.h
    cpu/arm64/Arm64BlockJit.cpp
    cpu/arm64/Arm64BlockJit.h
    cpu/arm64/cache/Arm64CacheFormat.cpp
    cpu/arm64/cache/Arm64CacheFormat.h
    platform/windows/Arm64CacheStorage.cpp
    platform/windows/Arm64CacheStorage.h
    cpu/arm64/Arm64ExecutionSession.cpp
    cpu/arm64/Arm64ExecutionSession.h
    loader/AndroidLoadedImage.h
    cpu/arm64/cache/Arm64BlockCache.cpp
    cpu/arm64/cache/Arm64BlockCache.h
    cpu/arm64/Arm64ToX64.cpp
    cpu/arm64/Arm64ToX64.h
    abi/x64/TlsInstructions.cpp
    abi/x64/TlsInstructions.h
    signals/X64TlsRecovery.cpp
    signals/X64TlsRecovery.h
    platform/windows/ExecutableMemory.cpp
    platform/windows/ExecutableMemory.h
    runtime/art/ArtGs.cpp
    runtime/art/ArtGs.h
    runtime/art/ArtJit.cpp
    runtime/art/ArtJit.h
    runtime/art/ArtAot.cpp
    runtime/art/ArtAot.h
    framework/ArtFramework.cpp
    framework/ArtFileObserver.cpp
    framework/ArtFileObserver.h
    framework/ArtDocumentTrees.cpp
    framework/ArtDocumentTrees.h
    platform/windows/DocumentTrees.cpp
    platform/windows/DocumentTrees.h
    framework/ArtAudio.cpp
    framework/ArtParcels.cpp
    framework/ArtFramework.h
    framework/ArtSurfaces.cpp
    framework/ArtSurfaces.h
    framework/ArtSurfaceTextures.cpp
    framework/ArtSurfaceTextures.h
    framework/ArtGl.cpp
    framework/ArtGl.h
    filesystem/AndroidPrivateLibraries.cpp
    filesystem/AndroidPrivateLibraries.h
    framework/ArtProcesses.cpp
    framework/ArtProcesses.h
    framework/ArtNotifications.cpp
    framework/ArtNotifications.h
    framework/ArtApplicationLauncher.cpp
    framework/ArtApplicationLauncher.h
    platform/windows/ArtSettingsConfirmation.cpp
    platform/windows/ArtSettingsConfirmation.h
    runtime/art/ArtJni.h
    runtime/art/ArtJni.cpp
    abi/Arm64HostJni.h
    abi/Arm64HostJni.cpp
    runtime/art/ArtRuntime.cpp
    runtime/art/ArtRuntime.h
    loader/ElfImage.cpp
    loader/ElfImage.h
    loader/ElfFormat.h

    hle/system/stub-libEGL.cpp
    hle/system/stub-libGLESv2.cpp
    hle/system/stub-libGLES-framework.cpp
    hle/system/stub-libOpenSLES.cpp
    hle/system/stub-libaaudio.cpp
    hle/system/stub-libandroid.cpp
    hle/system/stub-libandroid.h
    hle/system/stub-libandroid-fonts.cpp
    hle/system/stub-libandroid-fonts.h
    hle/system/stub-libandroid-hardware-buffer.cpp
    hle/system/stub-libandroid-hardware-buffer.h
    hle/system/stub-libandroid-input.cpp
    hle/system/stub-libandroid-input.h
    hle/system/stub-libandroid-looper.cpp
    hle/system/stub-libandroidicu.cpp
    hle/system/stub-libc.cpp
    hle/system/stub-libc-search.cpp
    hle/system/stub-libc-resolver.cpp
    hle/system/stub-libc.h
    hle/system/stub-libc-properties.h
    hle/system/stub-libc-users.h
    memory/GuestMappings.h
    hle/system/stub-libc-bionic.cpp
    "${PROJECT_SOURCE_DIR}/oss/bionic/errno-descriptions.inc"
    hle/system/stub-libc-posix.cpp
    hle/system/stub-libc-pthread.cpp
    hle/system/stub-libc-sockets.cpp
    hle/system/stub-libc-inotify.cpp
    hle/system/stub-libc-interfaces.cpp
    hle/system/stub-libc-interfaces.h
    hle/system/stub-libc-symlinks.cpp
    hle/system/stub-libc-symlinks.h
    hle/system/stub-libc-sockets.h
    hle/system/stub-libdl.cpp
    hle/system/stub-libdl.h
    hle/system/stub-libdl_android.cpp
    hle/system/stub-liblog.cpp
    hle/system/stub-liblog.h
    hle/system/stub-libm.cpp
    hle/system/stub-libnativeloader.cpp
    hle/system/stub-libnativeloader.h
    hle/system/stub-libmedia_jni.cpp
    hle/system/stub-libmedia_jni.h
    hle/system/stub-libmediandk-codec.cpp
    hle/system/stub-libmediandk-format.cpp
    hle/system/stub-libmediandk-format.h
    hle/system/stub-libmediandk-image-reader.cpp
    hle/system/stub-libmediandk-image-reader.h
    hle/system/stub-libsqlite.cpp
    hle/system/stub-libsqlite.h
    hle/system/stub-libsqlite-vfs.cpp
    hle/system/stub-libstdc++.cpp
    hle/system/stub-libvulkan.cpp
    hle/system/stub-libvulkan.h
    hle/system/stub-libz.cpp
    hle/system/stub-libz.h

    hle/external/LibraryJniStubs.h
    hle/external/stub-libclang_rt.ubsan_standalone.cpp
    hle/external/stub-libclang_rt.ubsan_standalone.h
    hle/external/stub-libcso.cpp
    hle/external/stub-libmatrix-jnihook.cpp
    hle/external/stub-libmmkv.cpp
    hle/external/stub-libwebviewchromium.cpp
    hle/external/stub-libwebviewchromium-draw.cpp
)
target_include_directories(stone-host PUBLIC
    "${PROJECT_SOURCE_DIR}/src/windows/service"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/hle/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/hle/external"
    "${PROJECT_SOURCE_DIR}/oss/vulkan/include"
)
target_link_libraries(stone-host PUBLIC bcrypt crypt32 cabinet onecore synchronization ws2_32 xmllite runtimeobject propsys)
add_executable(StoneArtCompiler runtime/art/ArtCompilerMain.cpp)
target_link_libraries(StoneArtCompiler PRIVATE stone-host)
target_compile_options(StoneArtCompiler PRIVATE /utf-8)
