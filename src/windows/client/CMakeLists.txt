add_executable(StoneClient main.cpp StoneClient.rc StoneClient.ico resource.h ../service/app.manifest)
target_include_directories(StoneClient PRIVATE
    "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(StoneClient PRIVATE stone-host)
set_target_properties(StoneClient PROPERTIES ENABLE_EXPORTS ON)
target_compile_features(StoneClient PRIVATE cxx_std_20)
target_compile_definitions(StoneClient PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(StoneClient PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8>")
if(STONE_WINDOWS_ARCH STREQUAL "x64")
    target_sources(StoneClient PRIVATE ArtLaunch.cpp ArtLaunch.h)
    add_dependencies(StoneClient stone-art-runtime stone-image-tools)
endif()

find_package(unofficial-angle CONFIG REQUIRED)
set(angle_package "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")

string(TOLOWER "${STONE_WINDOWS_ARCH}" angle_arch)
find_file(STONE_D3DCOMPILER NAMES d3dcompiler_47.dll
    HINTS "$ENV{WindowsSdkDir}/Redist/D3D/${angle_arch}"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]/Redist/D3D/${angle_arch}"
    NO_DEFAULT_PATH REQUIRED)
install(IMPORTED_RUNTIME_ARTIFACTS unofficial::angle::libEGL unofficial::angle::libGLESv2
    RUNTIME DESTINATION . COMPONENT stone-graphics)
install(FILES "${STONE_D3DCOMPILER}" DESTINATION . COMPONENT stone-graphics)
install(FILES "${angle_package}/angle/copyright" DESTINATION licenses/angle
    RENAME LICENSE COMPONENT stone-graphics)
install(FILES "${angle_package}/angle/vcpkg.spdx.json" DESTINATION licenses/angle COMPONENT stone-graphics)
foreach(dependency zlib egl-registry opengl-registry)
    install(FILES "${angle_package}/${dependency}/copyright" DESTINATION licenses/angle
        RENAME "${dependency}.txt" COMPONENT stone-graphics)
endforeach()
add_custom_target(stone-angle
    COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}" --config "$<CONFIG>"
        --prefix "${stone_platform_bin}/$<CONFIG>" --component stone-graphics
    COMMENT "Installing graphics runtime" USES_TERMINAL VERBATIM)
set_target_properties(stone-angle PROPERTIES FOLDER Dependencies)
add_dependencies(StoneClient stone-angle)
