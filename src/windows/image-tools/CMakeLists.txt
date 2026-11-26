# Only ZIP reading/inflate is retained; no external runtime or build-time download.
set(miniz_dir "${PROJECT_SOURCE_DIR}/oss/miniz")
configure_file(MinizExport.h.in "${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h" COPYONLY)
add_library(stone-image-zip STATIC "${miniz_dir}/miniz.c" "${miniz_dir}/miniz_tinfl.c" "${miniz_dir}/miniz_zip.c")
target_include_directories(stone-image-zip PUBLIC "${miniz_dir}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(stone-image-zip PUBLIC
    MINIZ_NO_ZLIB_APIS MINIZ_NO_DEFLATE_APIS MINIZ_NO_STDIO)
add_library(stone-image-core STATIC Image.h Image.cpp Ext4.cpp Erofs.cpp Metadata.cpp Preparation.cpp)
target_compile_features(stone-image-core PUBLIC cxx_std_20)
target_compile_definitions(stone-image-core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(stone-image-core PRIVATE /W4 /utf-8)
target_link_libraries(stone-image-core PUBLIC stone-image-zip ole32 user32)
add_executable(StoneImageTool main.cpp)
target_compile_options(StoneImageTool PRIVATE /W4 /utf-8)
target_link_libraries(StoneImageTool PRIVATE stone-image-core)
add_custom_command(TARGET StoneImageTool POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:StoneImageTool>/licenses/erofs-utils"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/oss/erofs-utils/LICENSE" "${PROJECT_SOURCE_DIR}/oss/erofs-utils/NOTICE"
        "$<TARGET_FILE_DIR:StoneImageTool>/licenses/erofs-utils"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${miniz_dir}/LICENSE"
        "$<TARGET_FILE_DIR:StoneImageTool>/licenses/miniz-MIT.txt" VERBATIM)
add_custom_target(stone-image-tools DEPENDS StoneImageTool)
set_target_properties(stone-image-zip stone-image-core StoneImageTool stone-image-tools PROPERTIES FOLDER windows)
