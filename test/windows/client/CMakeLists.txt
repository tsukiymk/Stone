add_executable(stone-runtime-features-tests runtime-features-tests.cpp)
target_compile_features(stone-runtime-features-tests PRIVATE cxx_std_20)
set_target_properties(stone-runtime-features-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/client/$<CONFIG>" FOLDER "Tests/Client")
add_test(NAME stone-runtime-features COMMAND "${STONE_TEST_POWERSHELL}" -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/RunRuntimeFeatures.ps1"
    -Executable "$<TARGET_FILE:stone-runtime-features-tests>"
    -OutputDirectory "${STONE_TEST_ROOT}/client/runtime-features")
set_tests_properties(stone-runtime-features PROPERTIES LABELS "client;unit" TIMEOUT 30)

add_executable(stone-sharing-tests WIN32 sharing-tests.cpp)
add_executable(stone-document-tests document-tests.cpp)
add_executable(stone-document-tree-tests document-tree-tests.cpp)
target_link_libraries(stone-document-tree-tests PRIVATE stone-host)
target_compile_options(stone-document-tree-tests PRIVATE /utf-8)
set_target_properties(stone-document-tree-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/client/$<CONFIG>")
add_test(NAME stone-document-trees COMMAND stone-document-tree-tests "${STONE_TEST_ROOT}/document-trees/$<CONFIG>")
set_tests_properties(stone-document-trees PROPERTIES LABELS "client;documents;unit" TIMEOUT 45)
target_link_libraries(stone-document-tests PRIVATE stone-documents)
target_include_directories(stone-document-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/windows/client")
target_compile_options(stone-document-tests PRIVATE /utf-8)
set_target_properties(stone-document-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/$<CONFIG>")
add_test(NAME stone-documents COMMAND stone-document-tests "${STONE_TEST_ROOT}/document-grants/$<CONFIG>")
set_tests_properties(stone-documents PROPERTIES LABELS "client;documents" TIMEOUT 30)
add_executable(stone-capture-protection-tests capture-protection-tests.cpp)
target_link_libraries(stone-capture-protection-tests PRIVATE stone-host d3d11 dxgi windowsapp)
target_compile_options(stone-capture-protection-tests PRIVATE /utf-8)
set_target_properties(stone-capture-protection-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/$<CONFIG>")
add_test(NAME stone-capture-protection COMMAND stone-capture-protection-tests)
add_test(NAME stone-window-capture COMMAND stone-capture-protection-tests --capture)
add_test(NAME stone-launch-screen COMMAND stone-capture-protection-tests --launch-screen)
set_tests_properties(stone-launch-screen PROPERTIES LABELS "client;window;startup" TIMEOUT 15)
set_tests_properties(stone-capture-protection stone-window-capture PROPERTIES LABELS "client;window;privacy" TIMEOUT 45)
set_tests_properties(stone-window-capture PROPERTIES SKIP_RETURN_CODE 77)
target_link_libraries(stone-sharing-tests PRIVATE stone-sharing shell32)
target_include_directories(stone-sharing-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/windows/client")
target_compile_options(stone-sharing-tests PRIVATE /utf-8)
set_target_properties(stone-sharing-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/$<CONFIG>")
add_test(NAME stone-sharing COMMAND stone-sharing-tests)
set_tests_properties(stone-sharing PROPERTIES LABELS "client;sharing" TIMEOUT 30)

add_executable(stone-settings-confirmation-tests settings-confirmation-tests.cpp)
target_link_libraries(stone-settings-confirmation-tests PRIVATE stone-host)
set_target_properties(stone-settings-confirmation-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/$<CONFIG>")
add_test(NAME stone-settings-confirmation COMMAND stone-settings-confirmation-tests)
set_tests_properties(stone-settings-confirmation PROPERTIES LABELS "client;settings" TIMEOUT 45)


stone_test_context()

set_property(TEST stone-documents stone-sharing APPEND PROPERTY LABELS unit)
