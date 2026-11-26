add_library(stone-xz STATIC
    xz_crc32.c xz_crc64.c xz_sha256.c xz_dec_stream.c xz_dec_lzma2.c xz_dec_bcj.c)
target_include_directories(stone-xz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(stone-xz PUBLIC XZ_USE_CRC64 PRIVATE XZ_USE_SHA256 XZ_DEC_X86)
set_target_properties(stone-xz PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
