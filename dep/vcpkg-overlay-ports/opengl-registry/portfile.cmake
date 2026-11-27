# The official port downloads the entire specification archive. Fetch the same
# commit's API/XML trees instead; ANGLE does not need the specification PDFs.
set(REF e8f7cd0e35ac8d6f5667a021ff83d04b1fec41ef)
set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src/git-${REF}")
vcpkg_find_acquire_program(GIT)
set(STONE_GIT "${GIT}")
if(NOT EXISTS "${SOURCE_PATH}/.git")
    vcpkg_execute_required_process(COMMAND "${STONE_GIT}" init "${SOURCE_PATH}"
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}" LOGNAME "git-init-${TARGET_TRIPLET}")
    vcpkg_execute_required_process(COMMAND "${STONE_GIT}" -C "${SOURCE_PATH}" remote add origin
        https://github.com/KhronosGroup/OpenGL-Registry.git
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}" LOGNAME "git-remote-${TARGET_TRIPLET}")
endif()
execute_process(COMMAND "${STONE_GIT}" -C "${SOURCE_PATH}" rev-parse HEAD
    OUTPUT_VARIABLE source_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT source_head STREQUAL REF)
    vcpkg_execute_required_process(COMMAND "${STONE_GIT}" -C "${SOURCE_PATH}"
        -c http.version=HTTP/1.1 -c http.lowSpeedLimit=1024 -c http.lowSpeedTime=120
        fetch --depth=1 --filter=blob:none origin "${REF}"
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}" LOGNAME "git-fetch-${TARGET_TRIPLET}")
    vcpkg_execute_required_process(COMMAND "${STONE_GIT}" -C "${SOURCE_PATH}" sparse-checkout set api xml
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}" LOGNAME "git-sparse-${TARGET_TRIPLET}")
    vcpkg_execute_required_process(COMMAND "${STONE_GIT}" -C "${SOURCE_PATH}"
        -c http.version=HTTP/1.1 checkout --detach "${REF}"
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}" LOGNAME "git-checkout-${TARGET_TRIPLET}")
endif()
execute_process(COMMAND "${STONE_GIT}" -C "${SOURCE_PATH}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE source_changes RESULT_VARIABLE source_status)
if(NOT source_status EQUAL 0 OR NOT source_changes STREQUAL "")
    message(FATAL_ERROR "OpenGL registry source contains local changes.")
endif()

# Preserve the official port's installed headers, XML, pkg-config and notice.
foreach(api GL GLES GLES2 GLES3 GLSC GLSC2)
    file(COPY "${SOURCE_PATH}/api/${api}" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
endforeach()
file(GLOB reg_files "${SOURCE_PATH}/xml/*.xml" "${SOURCE_PATH}/xml/readme.pdf"
    "${SOURCE_PATH}/xml/*.rnc" "${SOURCE_PATH}/xml/reg.py")
file(COPY ${reg_files} DESTINATION "${CURRENT_PACKAGES_DIR}/share/opengl")
vcpkg_install_copyright(FILE_LIST "${VCPKG_ROOT_DIR}/ports/opengl-registry/copyright")
set(pc_file [=[
prefix=${pcfiledir}/../..
datadir=${prefix}/share
specdir=${datadir}/opengl
Name: khronos-opengl-registry
Description: Khronos OpenGL registry
Version: git@REF@
]=])
string(CONFIGURE "${pc_file}" pc_file @ONLY)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/pkgconfig/khronos-opengl-registry.pc" "${pc_file}")
