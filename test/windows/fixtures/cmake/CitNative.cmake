foreach(input CLANG JNI_HEADER)
    if(NOT EXISTS "${${input}}")
        message(FATAL_ERROR "Native CIT requires ${input}: ${${input}}")
    endif()
endforeach()
set(native "${source}/native")
set(lib "${stage}/lib/x86_64")
file(MAKE_DIRECTORY "${lib}" "${stage}/lib/arm64-v8a")
tool("${CLANG}" --target=aarch64-linux-android23 -fuse-ld=lld -nostdlib -shared
    -Wl,-z,defs -Wl,--hash-style=gnu "${source}/nativejit/native_jit_arm64.S" -ldl -lm -lc
    -o "${stage}/lib/arm64-v8a/libnative_jit_test.so")
set(common --target=x86_64-linux-android23 -fuse-ld=lld -nostdlib -shared -fPIC
    -ffreestanding -fno-stack-protector -mno-red-zone -O2 -Wl,-z,defs -Wl,--hash-style=gnu)
tool("${CLANG}" ${common} -Wl,-soname,libmathdep.so -Wl,--pack-dyn-relocs=relr
    "${native}/mathdep.c" -o "${lib}/libmathdep.so")
foreach(pair "c|libc_link_stub.c" "dl|dl_link_stub.c" "m|math_link_stub.c" "android|android_link_stub.c")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 name)
    list(GET pair 1 file)
    tool("${CLANG}" ${common} "-Wl,-soname,lib${name}.so" "${native}/${file}" -o "${stage}/lib${name}.so")
endforeach()
tool("${CLANG}" ${common} -Wl,-soname,libdynamic.so "${native}/dynamic.c"
    "-L${lib}" -lmathdep -o "${lib}/libdynamic.so")
tool("${CLANG}" ${common} -Wl,-soname,libnative_test.so -Wl,--pack-dyn-relocs=android
    -include "${JNI_HEADER}" "${native}/native_test.c" "-L${lib}" -lmathdep
    "${stage}/libc.so" "${stage}/libdl.so" "${stage}/libm.so" "${stage}/libandroid.so"
    -o "${lib}/libnative_test.so")
tool("${jar}" --update --file "${result}" -C "${stage}" lib/x86_64/libmathdep.so
    -C "${stage}" lib/x86_64/libnative_test.so -C "${stage}" lib/x86_64/libdynamic.so
    -C "${stage}" lib/arm64-v8a/libnative_jit_test.so)
