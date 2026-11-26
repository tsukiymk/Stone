set(oss_dir "${PROJECT_SOURCE_DIR}/oss")
set(sqlite_dir "${oss_dir}/sqlite")

add_library(stone-host OBJECT
    graphics/AngleState.h
    ../subsystem/platform/windows/GlSurfaceJni.cpp
    ../subsystem/platform/windows/GlSurfaceJni.h
    media/MediaDecoder.cpp
    media/MediaDecoderState.h
    audio/WaveOutTrack.cpp
    audio/WaveOutTrack.h
    files/FolderPicker.cpp
    files/FolderPicker.h
    files/BinderHandles.cpp
    files/BinderHandles.h
    ../client/HostConfiguration.cpp
    ../client/HostConfiguration.h
    ../subsystem/platform/windows/AudioTrack.cpp
    audio/AudioOutput.cpp
    audio/AudioOutput.h
    ../subsystem/platform/windows/DocumentPicker.cpp
    ../subsystem/platform/windows/DocumentPicker.h
    graphics/GlSurface.cpp
    graphics/GlSurface.h
    ../subsystem/platform/windows/ImageCodec.cpp
    ../subsystem/platform/windows/MediaDecoder.cpp
    media/ArtMediaDecoder.cpp
    media/ArtMediaDecoder.h
    ProcessLog.cpp
    ProcessLog.h
    UserPaths.h
    ../subsystem/platform/windows/WallpaperBridge.cpp
    ../client/Window.cpp
    ../client/Window.h
    ../client/LaunchScreen.cpp
    ../client/LaunchScreen.h
    ../subsystem/platform/windows/WindowsBinderFiles.cpp
    ../subsystem/platform/windows/WindowsBinderFiles.h
    ../subsystem/platform/windows/WindowsIsolatedProcess.cpp
    ../subsystem/platform/windows/WindowsIsolatedProcess.h
)
add_library(stone-shell STATIC shell/ShellIntegration.cpp shell/ShellIntegration.h shell/NotificationRegistration.cpp shell/WebLinks.cpp shell/WebLinks.h)
set_source_files_properties(../client/LaunchScreen.cpp PROPERTIES COMPILE_OPTIONS /utf-8)
add_library(stone-sharing STATIC ../client/Sharing.cpp ../client/Sharing.h)
add_library(stone-documents STATIC ../client/Documents.cpp ../client/Documents.h)
target_compile_features(stone-documents PUBLIC cxx_std_20)
target_compile_definitions(stone-documents PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stone-documents PRIVATE /W4 /utf-8)
target_link_libraries(stone-documents PUBLIC ole32 shell32 user32 runtimeobject)
target_compile_features(stone-sharing PUBLIC cxx_std_20)
target_compile_definitions(stone-sharing PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stone-sharing PRIVATE /W4 /utf-8)
target_link_libraries(stone-sharing PUBLIC bcrypt ole32 runtimeobject user32)
target_compile_features(stone-shell PUBLIC cxx_std_20)
target_compile_definitions(stone-shell PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stone-shell PRIVATE /W4 /utf-8)
target_link_libraries(stone-shell PUBLIC advapi32 bcrypt ole32 propsys runtimeobject shell32 user32 windowscodecs)
target_compile_features(stone-host PUBLIC cxx_std_20)
target_compile_definitions(stone-host PUBLIC
    "$<$<COMPILE_LANGUAGE:C,CXX>:WIN32_LEAN_AND_MEAN;NOMINMAX;UNICODE;_UNICODE>"
)
target_compile_options(stone-host PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:/W4>")

target_include_directories(stone-host PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src/windows/service"
    "${PROJECT_SOURCE_DIR}/src/windows/client"
    "${STONE_JDK_HOME}/include"
    "${STONE_JDK_HOME}/include/win32"
    "${oss_dir}/angle/include"
    "${sqlite_dir}"
)
target_link_libraries(stone-host PUBLIC
    advapi32 comctl32 delayimp dwmapi user32 gdi32 icu mfplat mfuuid ole32 propsys shell32 shlwapi userenv uxtheme windowscodecs winmm wmcodecdspuuid
)
target_link_options(stone-host INTERFACE /DELAYLOAD:mfplat.dll)
