set(STONE_TEST_ROOT "${PROJECT_SOURCE_DIR}/bin/${STONE_WINDOWS_ARCH}/Debug/test")
file(MAKE_DIRECTORY "${STONE_TEST_ROOT}")
set(STONE_WINDOWS_TEST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
# Each component calls this after registering its tests. Preserve per-test
# ART variables while keeping all native and Java temporary files local.
function(stone_test_context)
    file(MAKE_DIRECTORY "${STONE_TEST_ROOT}/temp")
    get_property(component_tests DIRECTORY PROPERTY TESTS)
    if(component_tests)
        set_tests_properties(${component_tests} PROPERTIES WORKING_DIRECTORY "${STONE_TEST_ROOT}")
        set_property(TEST ${component_tests} APPEND PROPERTY ENVIRONMENT
            "TEMP=${STONE_TEST_ROOT}/temp" "TMP=${STONE_TEST_ROOT}/temp"
            "LOCALAPPDATA=${STONE_TEST_ROOT}/local" "STONE_LOG_DIRECTORY=${STONE_TEST_ROOT}/Logs")
    endif()
endfunction()
find_program(STONE_TEST_POWERSHELL NAMES pwsh powershell REQUIRED)
add_test(NAME stone-package-version-tests COMMAND "${STONE_TEST_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/packaging/CheckVersions.ps1")
set_tests_properties(stone-package-version-tests PROPERTIES LABELS unit)
add_test(NAME stone-certificate-gpg-tests COMMAND "${STONE_TEST_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/packaging/CheckCertificateGpg.ps1")
set_tests_properties(stone-certificate-gpg-tests PROPERTIES LABELS unit TIMEOUT 60)
add_test(NAME stone-angle-runtime-tests COMMAND "${STONE_TEST_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/integration/packaging/CheckAngleRuntime.ps1"
    -RuntimeDirectory "${PROJECT_SOURCE_DIR}/bin/x64/Debug")
set_tests_properties(stone-angle-runtime-tests PROPERTIES LABELS graphics TIMEOUT 60)
stone_test_context()
add_subdirectory(fixtures)
add_subdirectory(image-tools)

add_subdirectory(subsystem)
add_subdirectory(service)
add_subdirectory(ipc)
add_subdirectory(client)
add_subdirectory(host)
if(STONE_BUILD_SETTINGS)
    add_subdirectory(settings)
endif()

# Compiled test programs follow the owning Windows component.
foreach(target stone-art-probe stone-art-aot-tests stone-arm64-cache-tests stone-sqlite-wal-tests)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/$<CONFIG>" FOLDER "Tests/Subsystem")
endforeach()
foreach(target stone-host-services-tests stone-application-log-tests stone-job-queue-tests stone-job-profile-tests stone-pending-intents-tests stone-activity-route-tests stone-clipboard-tests)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/service/$<CONFIG>" FOLDER "Tests/Service")
endforeach()
foreach(target stone-sharing-tests stone-document-tests stone-document-tree-tests stone-capture-protection-tests stone-settings-confirmation-tests)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/client/$<CONFIG>" FOLDER "Tests/Client")
endforeach()
foreach(target stone-shell-icon-tests stone-shell-integration-tests)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/host/$<CONFIG>" FOLDER "Tests/Host")
endforeach()

# CTest writes its own Testing/ logs beside this entry file. Keep those logs in
# Debug too, while reusing the generated test definitions in the build tree.
file(GENERATE OUTPUT "${STONE_TEST_ROOT}/ctest/$<CONFIG>/CTestTestfile.cmake"
    CONTENT "subdirs(\"${CMAKE_CURRENT_BINARY_DIR}\")\n")

add_custom_target(stone-windows-tests DEPENDS
    stone-runtime-features-tests
    stone-art-string-tests
    stone-art-package-compatibility-tests
    stone-core-tests stone-art-probe stone-art-aot-tests stone-arm64-cache-tests
    stone-sqlite-wal-tests stone-image-classpath-tests
    stone-ipc-tests stone-host-services-tests stone-application-log-tests
    stone-job-queue-tests stone-job-profile-tests stone-pending-intents-tests
    stone-activity-route-tests stone-clipboard-tests stone-sharing-tests
    stone-document-tests stone-document-tree-tests stone-capture-protection-tests stone-settings-confirmation-tests
    stone-shell-icon-tests stone-shell-integration-tests stone-process-log-tests)
if(TARGET stone-image-download-tests)
    add_dependencies(stone-windows-tests stone-settings-package-ui-tests)
    add_dependencies(stone-windows-tests stone-image-download-tests)
endif()
if(TARGET stone-settings-ui-tests)
    add_dependencies(stone-windows-tests stone-settings-ui-tests)
endif()
