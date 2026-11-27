set(art_string_classes "${STONE_TEST_ROOT}/subsystem/art-strings")
set(art_string_test "${art_string_classes}/com/trebledot/stone/runtime/ArtStringTests.class")
add_custom_command(OUTPUT "${art_string_test}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${art_string_classes}"
    COMMAND "${STONE_JDK_HOME}/bin/javac.exe" --release 8 -Xlint:-options -encoding UTF-8
        -cp "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar" -d "${art_string_classes}"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/art/string-tests.java"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/art/string-tests.java"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar"
    VERBATIM)
add_custom_target(stone-art-string-tests DEPENDS "${art_string_test}")
add_dependencies(stone-art-string-tests stone-art-runtime)
add_test(NAME stone-art-strings COMMAND "${STONE_JDK_HOME}/bin/java.exe"
    -cp "${art_string_classes};${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar"
    com.trebledot.stone.runtime.ArtStringTests)
set_tests_properties(stone-art-strings PROPERTIES LABELS "art;unit" TIMEOUT 30)
add_test(NAME stone-art-string-build COMMAND "${STONE_TEST_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/art/string-build-tests.ps1"
    -Compiler "${PROJECT_SOURCE_DIR}/tools/BuildArtStrings.ps1"
    -OutputDirectory "${STONE_TEST_ROOT}/run/art-string-build")
set_tests_properties(stone-art-string-build PROPERTIES LABELS "art;unit" TIMEOUT 30)

set(art_compatibility_classes "${STONE_TEST_ROOT}/subsystem/art-package-compatibility")
set(art_compatibility_test "${art_compatibility_classes}/com/trebledot/stone/runtime/ArtPackageCompatibilityTests.class")
add_custom_command(OUTPUT "${art_compatibility_test}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${art_compatibility_classes}"
    COMMAND "${STONE_JDK_HOME}/bin/javac.exe" --release 8 -Xlint:-options -encoding UTF-8
        -cp "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar" -d "${art_compatibility_classes}"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/art/package-compatibility-tests.java"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/runtime/art/package-compatibility-tests.java"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar"
    VERBATIM)
add_custom_target(stone-art-package-compatibility-tests DEPENDS "${art_compatibility_test}")
add_dependencies(stone-art-package-compatibility-tests stone-art-runtime)
add_test(NAME stone-art-package-compatibility COMMAND "${STONE_JDK_HOME}/bin/java.exe"
    -cp "${art_compatibility_classes};${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar"
    com.trebledot.stone.runtime.ArtPackageCompatibilityTests "${STONE_TEST_ROOT}/run/art-package-compatibility")
set_tests_properties(stone-art-package-compatibility PROPERTIES LABELS "art;packages;unit" TIMEOUT 30)

add_executable(stone-core-tests
    core-tests.cpp
    support/test-require.cpp
    hle/system/libandroid-tests.cpp
    hle/system/graphics-tests.cpp
    hle/system/libc-tests.cpp
    hle/system/local-listener-probe.cpp
    hle/system/media-tests.cpp
    image/image-library-tests.cpp
    runtime/art/art-runtime-tests.cpp
    ../host/audio/audio-loopback.cpp
    hle/external/library-tests.cpp
    loader/elf-tests.cpp
    abi/bridge-tests.cpp
    abi/arm64-data-cache-tests.cpp
    hle/system/arm64-api-tests.cpp
    signals/signal-tests.cpp
    memory/mapping-tests.cpp
    cpu/arm64/instruction-tests.cpp
    cpu/arm64/decode-tests.cpp
    runtime/art/tls-tests.cpp
    compatibility-tests.cpp
    cpu/arm64/block-tests.cpp
    cpu/arm64/profile-tests.cpp
    hle/system/zlib-tests.cpp
)
target_link_libraries(stone-core-tests PRIVATE stone-host iphlpapi mmdevapi)
target_include_directories(stone-core-tests PRIVATE "${STONE_WINDOWS_TEST_SOURCE_DIR}")
set_target_properties(stone-core-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/core/$<CONFIG>" FOLDER "Tests/Subsystem")
add_dependencies(stone-core-tests stone-image-tools stone-image-classpath-tests)

# These probes link only Stone's native host and ART backend. They neither build
# nor load the legacy shared/runtime JAR or its SQLite JNI implementation.
add_library(stone-art-test-support OBJECT
    runtime/art/test-client.cpp runtime/art/art-runtime-tests.cpp
    ../host/audio/audio-loopback.cpp support/test-require.cpp
    "${PROJECT_SOURCE_DIR}/src/windows/client/ArtLaunch.cpp")
target_compile_definitions(stone-art-test-support PRIVATE
    "STONE_TEST_SERVICE=L\"$<TARGET_FILE:StoneService>\"")
target_compile_options(stone-art-test-support PRIVATE /utf-8)
target_include_directories(stone-art-test-support PUBLIC "${STONE_WINDOWS_TEST_SOURCE_DIR}")
target_link_libraries(stone-art-test-support PUBLIC stone-host mmdevapi)
add_dependencies(stone-art-test-support StoneService)
set_target_properties(stone-art-test-support PROPERTIES FOLDER "Tests/Subsystem")

add_executable(stone-art-probe runtime/art/entry.cpp)
add_custom_target(stone-art-test-runtime
    COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}" --config "$<CONFIG>"
        --prefix "$<TARGET_FILE_DIR:stone-art-probe>" --component stone-graphics
    COMMENT "Installing graphics runtime for ART tests" USES_TERMINAL VERBATIM)
set_target_properties(stone-art-test-runtime PROPERTIES FOLDER "Tests/Subsystem")
add_dependencies(stone-art-probe stone-art-test-runtime)
add_custom_target(stone-art-webview-tests DEPENDS stone-art-probe stone-art-probe-dex stone-cit-apk stone-archive-fixtures)
set_target_properties(stone-art-webview-tests PROPERTIES FOLDER "Tests/Subsystem")
foreach(target stone-art-probe)
    target_compile_options(${target} PRIVATE /utf-8)
    target_link_libraries(${target} PRIVATE stone-art-test-support stone-host)
endforeach()

add_executable(stone-art-aot-tests runtime/art/aot-tests.cpp)
add_executable(stone-arm64-cache-tests cpu/arm64/cache/arm64-cache-tests.cpp)
target_link_libraries(stone-arm64-cache-tests PRIVATE stone-host)
target_compile_options(stone-arm64-cache-tests PRIVATE /utf-8)
set_target_properties(stone-arm64-cache-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test")
target_link_libraries(stone-art-aot-tests PRIVATE stone-host)
target_compile_options(stone-art-aot-tests PRIVATE /utf-8)
set_target_properties(stone-art-aot-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test")
target_compile_definitions(stone-core-tests PRIVATE "STONE_TEST_SERVICE=L\"$<TARGET_FILE:StoneService>\"")
add_dependencies(stone-core-tests StoneService)
set(STONE_CORE_ART_DIRECTORY "" CACHE PATH "Optional prepared Android x86_64 ART image directory")
set(STONE_CORE_ART_DEX "" CACHE FILEPATH "Optional DEX built by stone-art-probe-dex target")
set(STONE_CORE_ART_CIT_APK "" CACHE FILEPATH "Optional Stone CIT APK for ART framework construction and Handler checks")
set(STONE_CORE_ART_ARCHIVES "" CACHE PATH "Optional APK signature fixtures built by stone-archive-fixtures target")
set(STONE_CORE_ART_JOB_FIXTURE "" CACHE FILEPATH "Optional jobs.apk built by stone-job-fixtures target")
option(STONE_CORE_ART_GRAPHICS "Verify image libhwui software Bitmap/Canvas through ART" OFF)
option(STONE_CORE_ART_RESOURCES "Verify framework and APK resources through image libandroid_runtime" OFF)
option(STONE_CORE_ART_APPLICATION "Verify original APK Application and providers through the own ART host" OFF)
option(STONE_CORE_ART_WINDOW "Present the ART Activity dashboard in a real Stone Win32 window" OFF)
if(STONE_CORE_ART_WINDOW AND NOT STONE_CORE_ART_APPLICATION)
    message(FATAL_ERROR "The ART window check requires STONE_CORE_ART_APPLICATION=ON.")
endif()
if((STONE_CORE_ART_GRAPHICS OR STONE_CORE_ART_RESOURCES OR STONE_CORE_ART_APPLICATION) AND
        (NOT STONE_CORE_ART_DIRECTORY OR NOT STONE_CORE_ART_DEX OR NOT STONE_CORE_ART_CIT_APK))
    message(FATAL_ERROR "ART graphics/resources/application require prepared ART, probe DEX and CIT APK paths.")
endif()
add_executable(stone-sqlite-wal-tests hle/system/sqlite-wal-tests.cpp support/test-require.cpp)
target_link_libraries(stone-sqlite-wal-tests PRIVATE stone-host)
target_include_directories(stone-sqlite-wal-tests PRIVATE "${STONE_WINDOWS_TEST_SOURCE_DIR}")
set_target_properties(stone-sqlite-wal-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/$<CONFIG>")
add_dependencies(stone-core-tests stone-sqlite-wal-tests)
if(STONE_CORE_ART_DIRECTORY)
    add_test(NAME stone-sqlite-wal COMMAND stone-sqlite-wal-tests "${STONE_CORE_ART_DIRECTORY}/lib64/libsqlite.so"
        "${STONE_TEST_ROOT}/sqlite-wal/$<CONFIG>")
    set_tests_properties(stone-sqlite-wal PROPERTIES LABELS "core;sqlite;system-image" TIMEOUT 180)
endif()
if(STONE_CORE_ART_DIRECTORY AND STONE_CORE_ART_DEX)
    add_test(NAME stone-art-runtime COMMAND stone-art-probe
        "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}")
    set_tests_properties(stone-art-runtime PROPERTIES LABELS "core;art" TIMEOUT 120)
    add_test(NAME stone-art-data-mount COMMAND stone-art-probe
        "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}")
    set_tests_properties(stone-art-data-mount PROPERTIES LABELS "core;art;filesystem" TIMEOUT 120
        ENVIRONMENT "STONE_ART_DATA_DIRECTORY=${STONE_TEST_ROOT}/art-external-data")
    if(STONE_CORE_ART_CIT_APK)
        add_test(NAME stone-art-framework COMMAND stone-art-probe
            "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}")
        set_tests_properties(stone-art-framework PROPERTIES LABELS "core;art;framework" TIMEOUT 120)
        if(STONE_CORE_ART_GRAPHICS)
            add_test(NAME stone-art-graphics COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}" --graphics)
            set_tests_properties(stone-art-graphics PROPERTIES LABELS "core;art;graphics" TIMEOUT 120)
        endif()
        if(STONE_CORE_ART_RESOURCES)
            add_test(NAME stone-art-resources COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}" --resources)
            set_tests_properties(stone-art-resources PROPERTIES LABELS "core;art;resources" TIMEOUT 120)
        endif()
        if(STONE_CORE_ART_APPLICATION)
            add_dependencies(stone-art-probe stone-art-runtime)
            add_test(NAME stone-art-battery COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}"
                --battery "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
            set_tests_properties(stone-art-battery PROPERTIES LABELS "core;art;application;battery" TIMEOUT 180
                ENVIRONMENT "STONE_ART_DATA_DIRECTORY=${STONE_TEST_ROOT}/art-battery-cit")
            if(STONE_CORE_ART_JOB_FIXTURE)
                add_test(NAME stone-art-jobs COMMAND powershell.exe -NoProfile -File
                    "${STONE_WINDOWS_TEST_SOURCE_DIR}/integration/art/RunJobs.ps1" -Probe "$<TARGET_FILE:stone-art-probe>"
                    -Service "$<TARGET_FILE:StoneService>" -PreparedSystem "${STONE_CORE_ART_DIRECTORY}"
                    -ProbeDex "${STONE_CORE_ART_DEX}" -FixtureApk "${STONE_CORE_ART_JOB_FIXTURE}"
                    -ArtRuntimeJar "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar"
                    -OutputDirectory "${STONE_TEST_ROOT}/art-jobs-$<CONFIG>")
                set_tests_properties(stone-art-jobs PROPERTIES LABELS "core;art;application;jobs" TIMEOUT 300)
            endif()
            add_test(NAME stone-art-application COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}"
                --application "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
            set_tests_properties(stone-art-application PROPERTIES LABELS "core;art;application" TIMEOUT 180)
            add_test(NAME stone-art-startup-natives COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}"
                --startup-natives "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
            set_tests_properties(stone-art-startup-natives PROPERTIES LABELS "core;art;application;startup" TIMEOUT 60)
            add_test(NAME stone-art-webview-providers COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}"
                --webview-providers "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
            set_tests_properties(stone-art-webview-providers PROPERTIES LABELS "core;art;application;webview" TIMEOUT 90
                ENVIRONMENT "STONE_ART_DATA_DIRECTORY=${STONE_TEST_ROOT}/art-webview-providers")
            add_test(NAME stone-art-video-codecs COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}"
                --video-codecs "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
            set_tests_properties(stone-art-video-codecs PROPERTIES LABELS "core;art;application;media" TIMEOUT 90
                ENVIRONMENT "STONE_ART_DATA_DIRECTORY=${STONE_TEST_ROOT}/art-video-codecs")
            add_test(NAME stone-art-resize-window COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}"
                --resize-window "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
            set_tests_properties(stone-art-resize-window PROPERTIES LABELS "core;art;application;graphics" TIMEOUT 120
                ENVIRONMENT "STONE_ART_DATA_DIRECTORY=${STONE_TEST_ROOT}/art-resize-window")
            add_test(NAME stone-art-filesystem COMMAND stone-art-probe
                "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}"
                --application "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
            set_tests_properties(stone-art-filesystem PROPERTIES LABELS "core;art;filesystem" TIMEOUT 180
                ENVIRONMENT "STONE_ART_PROBE_ONLY_FILESYSTEM=1")
            if(STONE_CORE_ART_ARCHIVES)
                add_test(NAME stone-art-packages COMMAND stone-art-probe
                    "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_ARCHIVES}/launch.apk"
                    --application "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
                set_tests_properties(stone-art-packages PROPERTIES LABELS "core;art;application;packages" TIMEOUT 180
                    ENVIRONMENT "STONE_ART_PROBE_ARCHIVES=${STONE_CORE_ART_ARCHIVES};STONE_ART_PROBE_ONLY_PACKAGES=1;STONE_ART_DATA_DIRECTORY=${STONE_TEST_ROOT}/art-packages")
                add_test(NAME stone-art-archives COMMAND stone-art-probe
                    "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_ARCHIVES}/launch.apk"
                    --application "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
                set_tests_properties(stone-art-archives PROPERTIES LABELS "core;art;application;archives" TIMEOUT 180
                    ENVIRONMENT "STONE_ART_PROBE_ARCHIVES=${STONE_CORE_ART_ARCHIVES}")
                add_test(NAME stone-art-uninstall COMMAND stone-art-probe
                    "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_ARCHIVES}/launch.apk"
                    --application "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
                set_tests_properties(stone-art-uninstall PROPERTIES LABELS "core;art;application;uninstall" TIMEOUT 180
                    ENVIRONMENT "STONE_ART_PROBE_ARCHIVES=${STONE_CORE_ART_ARCHIVES};STONE_ART_PROBE_ONLY_UNINSTALL=1")
                add_test(NAME stone-art-launchers COMMAND stone-art-probe
                    "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_ARCHIVES}/launch.apk"
                    --application "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
                set_tests_properties(stone-art-launchers PROPERTIES LABELS "core;art;application;launchers" TIMEOUT 180
                    ENVIRONMENT "STONE_ART_PROBE_ARCHIVES=${STONE_CORE_ART_ARCHIVES};STONE_ART_PROBE_ONLY_LAUNCHERS=1")
            endif()
            if(STONE_CORE_ART_WINDOW)
                add_test(NAME stone-art-window COMMAND stone-art-probe
                    "${STONE_CORE_ART_DIRECTORY}" "${STONE_CORE_ART_DEX}" "${STONE_CORE_ART_CIT_APK}"
                    --application "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
                set_tests_properties(stone-art-window PROPERTIES LABELS "core;art;application;window" TIMEOUT 180
                    ENVIRONMENT "STONE_ART_PROBE_WINDOW=1")
            endif()
        endif()
    endif()
endif()

foreach(art_test stone-art-runtime stone-art-data-mount stone-art-framework stone-art-graphics stone-art-resources stone-art-application stone-art-filesystem stone-art-packages stone-art-archives stone-art-uninstall stone-art-launchers stone-art-window)
    if(TEST ${art_test})
        set_tests_properties(${art_test} PROPERTIES RESOURCE_LOCK stone-art-runtime)
    endif()
endforeach()

add_library(stone-test-egl SHARED ../host/graphics/angle-egl-fixture.cpp)
add_library(stone-test-gles SHARED ../host/graphics/angle-gles-fixture.cpp)
set_target_properties(stone-test-egl stone-test-gles PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/angle/$<CONFIG>")
set_target_properties(stone-test-egl PROPERTIES OUTPUT_NAME libEGL)
set_target_properties(stone-test-gles PROPERTIES OUTPUT_NAME libGLESv2)
add_dependencies(stone-core-tests stone-test-egl stone-test-gles)
add_custom_command(TARGET stone-core-tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:stone-test-egl>"
        "$<TARGET_FILE:stone-test-gles>" "$<TARGET_FILE_DIR:stone-core-tests>"
    VERBATIM)

set(test_environment
    "STONE_IMAGE_TOOLS=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/StoneImageTool.exe"
)

foreach(api 29 30 33 37)
    add_test(NAME stone-core-system-api-${api} COMMAND stone-core-tests system ${api})
    set_tests_properties(stone-core-system-api-${api} PROPERTIES
        LABELS "core;system;api${api}"
        ENVIRONMENT "${test_environment};STONE_SDK_INT=${api}"
        TIMEOUT 60)

    set(STONE_CORE_API${api}_IMAGE "" CACHE FILEPATH "Optional x86_64 Android API ${api} system image")
    if(STONE_CORE_API${api}_IMAGE)
        add_test(NAME stone-core-system-image-api-${api} COMMAND stone-core-tests
            system-image ${api} "${STONE_CORE_API${api}_IMAGE}")
        set_tests_properties(stone-core-system-image-api-${api} PROPERTIES
            LABELS "core;system-image;api${api}"
            ENVIRONMENT "${test_environment}"
            TIMEOUT 60)
    endif()
endforeach()

add_test(NAME stone-core-external COMMAND stone-core-tests external)
set_tests_properties(stone-core-external PROPERTIES LABELS "core;external" TIMEOUT 60)

foreach(group hle allocation local-listener-probe inotify-storage proc-cmdline external-local arm64-profile arm64-block arm64-dispatch)
    add_test(NAME stone-core-${group} COMMAND stone-core-tests ${group})
    set_tests_properties(stone-core-${group} PROPERTIES LABELS "core;native-layers;unit" TIMEOUT 60)
endforeach()

foreach(first environ getenv arm64)
    add_test(NAME stone-core-environment-${first} COMMAND stone-core-tests environment ${first})
    set_tests_properties(stone-core-environment-${first} PROPERTIES LABELS "core;libc;unit" TIMEOUT 30)
endforeach()

add_test(NAME stone-core-arm64-instructions COMMAND stone-core-tests arm64)
set_tests_properties(stone-core-arm64-instructions PROPERTIES LABELS "core;arm64" TIMEOUT 60)

set(check_catalog "${STONE_WINDOWS_TEST_SOURCE_DIR}/subsystem/hle/check-library-compatibility.ps1")
add_test(NAME stone-core-library-catalog COMMAND
    "${STONE_TEST_POWERSHELL}" -NoProfile -File "${check_catalog}")
set_tests_properties(stone-core-library-catalog PROPERTIES LABELS "core;catalog" TIMEOUT 60)

set(STONE_CORE_COMPATIBILITY_APKS "" CACHE STRING "Optional semicolon-separated APK fixtures for library hashing")
set(STONE_CORE_AAPT2 "" CACHE FILEPATH "Path to aapt2.exe for APK fixture metadata")
if(STONE_CORE_COMPATIBILITY_APKS AND NOT EXISTS "${STONE_CORE_AAPT2}")
    message(FATAL_ERROR "Set STONE_CORE_AAPT2 to aapt2.exe to check the configured APK fixtures.")
endif()
set(apk_index 0)
foreach(apk IN LISTS STONE_CORE_COMPATIBILITY_APKS)
    math(EXPR apk_index "${apk_index} + 1")
    add_test(NAME stone-core-library-apk-${apk_index} COMMAND
        "${STONE_TEST_POWERSHELL}" -NoProfile -File "${check_catalog}" -Apk "${apk}" -Aapt2 "${STONE_CORE_AAPT2}")
    set_tests_properties(stone-core-library-apk-${apk_index} PROPERTIES
        LABELS "core;catalog" TIMEOUT 60)
endforeach()

set(STONE_CORE_UBSAN_ELF "" CACHE FILEPATH "Optional Android 37 x86_64 UBSan ELF with the pinned build ID")
if(STONE_CORE_UBSAN_ELF)
    add_test(NAME stone-core-external-ubsan-android-37 COMMAND stone-core-tests
        ubsan "${STONE_CORE_UBSAN_ELF}")
    set_tests_properties(stone-core-external-ubsan-android-37 PROPERTIES LABELS "core;external" TIMEOUT 60)
endif()


stone_test_context()

set_property(TEST stone-core-arm64-instructions stone-core-library-catalog APPEND PROPERTY LABELS unit)
