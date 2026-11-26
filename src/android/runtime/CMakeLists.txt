# The ART application host compiles only Stone-owned sources against the SDK.
# D8 is a build tool; the resulting archive is executed by the selected ART.
set(stone_android_sdk "$ENV{ANDROID_HOME}")
if(NOT stone_android_sdk)
    set(stone_android_sdk "$ENV{ANDROID_SDK_ROOT}")
endif()
if(NOT stone_android_sdk)
    set(stone_android_sdk "$ENV{USERPROFILE}/AppData/Local/Android/Sdk")
endif()
set(STONE_ANDROID_SDK "${stone_android_sdk}" CACHE PATH "Android SDK used to build the clean ART application host")
set(STONE_ART_ANDROID_JAR "${STONE_ANDROID_SDK}/platforms/android-37.0/android.jar" CACHE FILEPATH "Android framework API used to compile the ART host")
set(STONE_ART_D8_JAR "${STONE_ANDROID_SDK}/build-tools/36.0.0/lib/d8.jar" CACHE FILEPATH "D8 build-tool archive for the ART host")

set(art_runtime_jar "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/stone-art-runtime.jar")
file(GLOB_RECURSE art_runtime_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/java/*.java")
file(GLOB_RECURSE art_runtime_compile_api CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/compile/*.java")
file(GLOB_RECURSE art_runtime_resources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/*.xml")
add_custom_command(
    OUTPUT "${art_runtime_jar}"
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${PROJECT_SOURCE_DIR}/tools/BuildArtRuntime.ps1"
        -JdkHome "${STONE_JDK_HOME}" -AndroidJar "${STONE_ART_ANDROID_JAR}" -D8Jar "${STONE_ART_D8_JAR}"
        -Output "${art_runtime_jar}"
    DEPENDS ${art_runtime_sources} ${art_runtime_compile_api} ${art_runtime_resources}
        "${PROJECT_SOURCE_DIR}/tools/BuildArtRuntime.ps1" "${PROJECT_SOURCE_DIR}/tools/BuildArtStrings.ps1" "${CMAKE_CURRENT_LIST_FILE}"
        "${STONE_JDK_HOME}/bin/javac.exe" "${STONE_JDK_HOME}/bin/jar.exe" "${STONE_JDK_HOME}/bin/java.exe"
        "${STONE_ART_ANDROID_JAR}" "${STONE_ART_D8_JAR}"
    COMMENT "Building the standalone Stone ART application host"
    VERBATIM)
add_custom_target(stone-art-runtime DEPENDS "${art_runtime_jar}")
