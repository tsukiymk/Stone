function(stone_find_windows_jdk21 output_variable)
    # Inspect every PATH entry so an earlier JRE or another JDK version does
    # not hide a usable JDK 21 later in PATH.
    cmake_path(CONVERT "$ENV{PATH}" TO_CMAKE_PATH_LIST stone_java_paths NORMALIZE)
    set(stone_jdk_candidates)
    foreach(stone_java_path IN LISTS stone_java_paths)
        if(EXISTS "${stone_java_path}/javac.exe")
            get_filename_component(stone_jdk_candidate "${stone_java_path}/.." ABSOLUTE)
            list(APPEND stone_jdk_candidates "${stone_jdk_candidate}")
        endif()
    endforeach()

    foreach(stone_java_registry_key IN ITEMS
            "HKLM/SOFTWARE/JavaSoft/JDK"
            "HKLM/SOFTWARE/JavaSoft/Java Development Kit")
        cmake_host_system_information(RESULT stone_java_registry_versions
            QUERY WINDOWS_REGISTRY "${stone_java_registry_key}" SUBKEYS VIEW HOST
            ERROR_VARIABLE stone_java_registry_error)
        if(NOT stone_java_registry_error)
            foreach(stone_java_registry_version IN LISTS stone_java_registry_versions)
                cmake_host_system_information(RESULT stone_java_registry_home
                    QUERY WINDOWS_REGISTRY "${stone_java_registry_key}/${stone_java_registry_version}"
                    VALUE JavaHome VIEW HOST ERROR_VARIABLE stone_java_registry_error)
                if(NOT stone_java_registry_error AND NOT stone_java_registry_home STREQUAL "")
                    list(APPEND stone_jdk_candidates "${stone_java_registry_home}")
                endif()
            endforeach()
        endif()
    endforeach()

    # Cover common machine-wide and per-user installations without scanning
    # whole drives. The build uses a host JDK even when targeting ARM64.
    set(stone_java_install_roots "$ENV{ProgramW6432}" "$ENV{ProgramFiles}")
    if(NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
        list(APPEND stone_java_install_roots "$ENV{LOCALAPPDATA}/Programs")
    endif()
    foreach(stone_java_install_root IN LISTS stone_java_install_roots)
        if(stone_java_install_root STREQUAL "")
            continue()
        endif()
        foreach(stone_java_vendor IN ITEMS
                "Java" "Eclipse Adoptium" "Microsoft" "Amazon Corretto"
                "BellSoft" "Zulu" "Semeru")
            file(GLOB stone_java_installations LIST_DIRECTORIES TRUE
                "${stone_java_install_root}/${stone_java_vendor}/*")
            list(SORT stone_java_installations COMPARE NATURAL ORDER DESCENDING)
            list(APPEND stone_jdk_candidates ${stone_java_installations})
        endforeach()
    endforeach()
    if(NOT "$ENV{USERPROFILE}" STREQUAL "")
        file(GLOB stone_java_user_installations LIST_DIRECTORIES TRUE
            "$ENV{USERPROFILE}/.jdks/*" "$ENV{USERPROFILE}/scoop/apps/*/current")
        list(SORT stone_java_user_installations COMPARE NATURAL ORDER DESCENDING)
        list(APPEND stone_jdk_candidates ${stone_java_user_installations})
    endif()

    list(REMOVE_DUPLICATES stone_jdk_candidates)
    foreach(stone_jdk_candidate IN LISTS stone_jdk_candidates)
        if(NOT EXISTS "${stone_jdk_candidate}/bin/javac.exe" OR
           NOT EXISTS "${stone_jdk_candidate}/bin/jar.exe" OR
           NOT EXISTS "${stone_jdk_candidate}/include/win32/jni_md.h")
            continue()
        endif()
        execute_process(COMMAND "${stone_jdk_candidate}/bin/javac.exe" -version
            RESULT_VARIABLE stone_java_result
            OUTPUT_VARIABLE stone_java_version
            ERROR_VARIABLE stone_java_error
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE
            TIMEOUT 10)
        if(stone_java_result STREQUAL "0" AND stone_java_version MATCHES "^javac 21(\\.|$)")
            set(${output_variable} "${stone_jdk_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${output_variable} "" PARENT_SCOPE)
endfunction()

if(NOT DEFINED STONE_JDK_HOME)
    if(NOT "$ENV{JAVA_HOME}" STREQUAL "")
        set(STONE_JDK_HOME "$ENV{JAVA_HOME}")
    else()
        stone_find_windows_jdk21(STONE_JDK_HOME)
        if(NOT STONE_JDK_HOME STREQUAL "")
            message(STATUS "Found Windows JDK 21: ${STONE_JDK_HOME}")
        endif()
    endif()
endif()

if(STONE_JDK_HOME STREQUAL "" OR
   NOT EXISTS "${STONE_JDK_HOME}/bin/javac.exe" OR
   NOT EXISTS "${STONE_JDK_HOME}/bin/jar.exe" OR
   NOT EXISTS "${STONE_JDK_HOME}/include/win32/jni_md.h")
    message(FATAL_ERROR
        "A Windows JDK 21 is required. Current STONE_JDK_HOME: '${STONE_JDK_HOME}'.\n"
        "If automatic discovery cannot find JDK 21, set JAVA_HOME to your Windows JDK 21 installation "
        "before configuring a fresh build, "
        "or pass -DSTONE_JDK_HOME=<jdk-directory> when running CMake.")
endif()
execute_process(
    COMMAND "${STONE_JDK_HOME}/bin/javac.exe" -version
    RESULT_VARIABLE stone_javac_result
    OUTPUT_VARIABLE stone_javac_version
    ERROR_VARIABLE stone_javac_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT stone_javac_result STREQUAL "0" OR NOT stone_javac_version MATCHES "^javac 21(\\.|$)")
    message(FATAL_ERROR "Stone requires JDK 21. javac reported: ${stone_javac_version} ${stone_javac_error}")
endif()
set(STONE_JDK_HOME "${STONE_JDK_HOME}" CACHE PATH "Windows JDK 21 used for build-time JNI headers and ART bootstrap DEX")
