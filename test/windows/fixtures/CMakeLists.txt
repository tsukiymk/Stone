# Fixture builds are explicit targets. They never start an APK or a Windows UI.
set(STONE_CIT_NATIVE OFF CACHE BOOL "Include the existing NDK JNI checks in the CIT fixture")
set(STONE_TEST_NDK_CLANG "" CACHE FILEPATH "Windows NDK clang.exe for optional CIT native fixtures")
set(STONE_TEST_NDK_JNI_HEADER "" CACHE FILEPATH "NDK sysroot jni.h for optional CIT native fixtures")
set(STONE_TEST_SAMPLE_APK "" CACHE FILEPATH "Optional signed APK copied and recorded by archive fixtures")
if(STONE_TEST_NDK_CLANG AND NOT STONE_TEST_NDK_JNI_HEADER)
    get_filename_component(ndk_bin "${STONE_TEST_NDK_CLANG}" DIRECTORY)
    get_filename_component(ndk_root "${ndk_bin}" DIRECTORY)
    set(STONE_TEST_NDK_JNI_HEADER "${ndk_root}/sysroot/usr/include/jni.h")
endif()
set(stone_fixture_root "${CMAKE_CURRENT_SOURCE_DIR}")
set(STONE_FIXTURE_OUTPUT "${STONE_TEST_ROOT}/fixtures")
file(GLOB_RECURSE stone_fixture_inputs CONFIGURE_DEPENDS
    "${stone_fixture_root}/*.java" "${stone_fixture_root}/*.xml" "${stone_fixture_root}/*.xml.in"
    "${stone_fixture_root}/*.png" "${stone_fixture_root}/*.c" "${stone_fixture_root}/*.S"
    "${stone_fixture_root}/*.html" "${stone_fixture_root}/*.js" "${stone_fixture_root}/*.txt"
    "${stone_fixture_root}/*.cmake")

function(stone_fixture target mode directory)
    set(output "${STONE_FIXTURE_OUTPUT}/${directory}")
    set(products "${output}/built.stamp")
    foreach(product IN LISTS ARGN)
        list(APPEND products "${output}/${product}")
    endforeach()
    set(config "${CMAKE_CURRENT_BINARY_DIR}/${target}-$<CONFIG>.inputs")
    file(GENERATE OUTPUT "${config}" CONTENT
        "${STONE_JDK_HOME}\n${STONE_ART_ANDROID_JAR}\n${STONE_ART_D8_JAR}\n${STONE_CIT_NATIVE}\n${STONE_TEST_NDK_CLANG}\n${STONE_TEST_NDK_JNI_HEADER}\n${STONE_TEST_SAMPLE_APK}\n")
    set(extra_depends)
    if(mode STREQUAL "probe")
        list(APPEND extra_depends "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
    elseif(mode STREQUAL "archives")
        list(APPEND extra_depends "${STONE_FIXTURE_OUTPUT}/cit/stone-cit.apk")
        if(STONE_TEST_SAMPLE_APK)
            list(APPEND extra_depends "${STONE_TEST_SAMPLE_APK}")
        endif()
    endif()
    add_custom_command(OUTPUT ${products}
        COMMAND "${CMAKE_COMMAND}"
            "-DMODE=${mode}" "-DCONFIG=$<CONFIG>" "-DJDK=${STONE_JDK_HOME}"
            "-DANDROID_JAR=${STONE_ART_ANDROID_JAR}" "-DD8_JAR=${STONE_ART_D8_JAR}"
            "-DOUTPUT=${output}" "-DDEBUG_ROOT=${PROJECT_SOURCE_DIR}/bin/${STONE_WINDOWS_ARCH}/Debug"
            "-DFIXTURE_ROOT=${stone_fixture_root}"
            "-DRUNTIME_JAR=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar"
            "-DCIT_APK=${STONE_FIXTURE_OUTPUT}/cit/stone-cit.apk"
            "-DCIT_NATIVE=${STONE_CIT_NATIVE}" "-DCLANG=${STONE_TEST_NDK_CLANG}"
            "-DJNI_HEADER=${STONE_TEST_NDK_JNI_HEADER}" "-DSAMPLE_APK=${STONE_TEST_SAMPLE_APK}"
            -P "${stone_fixture_root}/cmake/BuildFixtures.cmake"
        DEPENDS ${stone_fixture_inputs} ${extra_depends} "${config}" "${CMAKE_CURRENT_LIST_FILE}"
            "${STONE_JDK_HOME}/bin/javac.exe" "${STONE_JDK_HOME}/bin/java.exe"
            "${STONE_JDK_HOME}/bin/jar.exe" "${STONE_ART_ANDROID_JAR}" "${STONE_ART_D8_JAR}"
        COMMENT "Building ${mode} Android test fixtures with CMake"
        VERBATIM)
    add_custom_target(${target} DEPENDS ${products})
    set_property(GLOBAL APPEND PROPERTY STONE_FIXTURE_TARGETS ${target})
endfunction()

stone_fixture(stone-art-core-dex core-probe art-core classes.dex)
stone_fixture(stone-art-probe-dex probe art-probe classes.dex)
add_dependencies(stone-art-probe-dex stone-art-runtime)
add_executable(stone-avc-fixture-generator tools/GenerateAvc.cpp)
add_executable(stone-aac-fixture-generator tools/GenerateAac.cpp)
target_compile_features(stone-aac-fixture-generator PRIVATE cxx_std_20)
target_compile_definitions(stone-aac-fixture-generator PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(stone-aac-fixture-generator PRIVATE mfplat mfuuid ole32 wmcodecdspuuid)
set_target_properties(stone-aac-fixture-generator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/fixtures/art-video/$<CONFIG>" FOLDER "Tests/Fixtures")
target_compile_features(stone-avc-fixture-generator PRIVATE cxx_std_20)
target_compile_definitions(stone-avc-fixture-generator PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(stone-avc-fixture-generator PRIVATE mfplat mfuuid ole32 wmcodecdspuuid)
set_target_properties(stone-avc-fixture-generator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/fixtures/art-video/$<CONFIG>" FOLDER "Tests/Fixtures")
add_custom_target(stone-art-video-fixture
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${STONE_TEST_ROOT}/fixtures/art-probe"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${STONE_TEST_ROOT}/fixtures/art-video/temp"
    COMMAND "${CMAKE_COMMAND}" -E env "TEMP=${STONE_TEST_ROOT}/fixtures/art-video/temp"
        "TMP=${STONE_TEST_ROOT}/fixtures/art-video/temp"
        "$<TARGET_FILE:stone-avc-fixture-generator>" "${STONE_TEST_ROOT}/fixtures/art-probe/avc-fixture.bin"
    COMMAND "${CMAKE_COMMAND}" -E env "TEMP=${STONE_TEST_ROOT}/fixtures/art-video/temp"
        "TMP=${STONE_TEST_ROOT}/fixtures/art-video/temp"
        "$<TARGET_FILE:stone-aac-fixture-generator>" "${STONE_TEST_ROOT}/fixtures/art-probe/aac-fixture.bin"
    WORKING_DIRECTORY "${STONE_TEST_ROOT}"
    DEPENDS stone-avc-fixture-generator stone-aac-fixture-generator VERBATIM)
set_property(GLOBAL APPEND PROPERTY STONE_FIXTURE_TARGETS stone-art-video-fixture)
stone_fixture(stone-cit-apk cit cit stone-cit.apk)
stone_fixture(stone-job-fixtures jobs jobs jobs.apk)
stone_fixture(stone-foreground-fixtures foreground foreground foreground.apk)
stone_fixture(stone-activity-route-fixtures activity-routes activity-routes source.apk target.apk)
stone_fixture(stone-resize-fixtures resize resize default-23.apk default-24.apk app-false.apk
    activity-true.apk activity-false.apk resource-true.apk alias.apk)
stone_fixture(stone-archive-fixtures archives archives expected.properties launch.apk v1.apk v2.apk v3.apk
    future.apk native-32.apk
    rotated.apk rotated-no-data.apk multi.apk unsigned.apk truncated.apk
    tampered-v1.apk tampered-v2.apk tampered-v3.apk tampered-rotated.apk tampered-multi.apk)
add_dependencies(stone-archive-fixtures stone-cit-apk)
get_property(fixture_targets GLOBAL PROPERTY STONE_FIXTURE_TARGETS)
add_custom_target(stone-test-fixtures DEPENDS ${fixture_targets})
