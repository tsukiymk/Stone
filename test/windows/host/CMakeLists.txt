add_executable(stone-process-log-tests process-log-tests.cpp "${PROJECT_SOURCE_DIR}/src/windows/host/ProcessLog.cpp")
target_link_libraries(stone-process-log-tests PRIVATE stone-ipc)
target_compile_options(stone-process-log-tests PRIVATE /utf-8)
set_target_properties(stone-process-log-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/host/$<CONFIG>" FOLDER "Tests/Host")
add_test(NAME stone-process-logs COMMAND stone-process-log-tests "${STONE_TEST_ROOT}/process-logs/$<CONFIG>")
set_tests_properties(stone-process-logs PROPERTIES LABELS "host;unit" TIMEOUT 30)

add_executable(stone-shell-icon-tests shell/shell-icon-tests.cpp)
target_include_directories(stone-shell-icon-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/windows/service")
target_compile_features(stone-shell-icon-tests PRIVATE cxx_std_20)
set_target_properties(stone-shell-icon-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/$<CONFIG>")
add_test(NAME stone-shell-icons COMMAND stone-shell-icon-tests)
set_tests_properties(stone-shell-icons PROPERTIES LABELS "host;shell" TIMEOUT 30)

add_executable(stone-shell-integration-tests shell/shell-integration-tests.cpp)
target_link_libraries(stone-shell-integration-tests PRIVATE stone-shell stone-ipc)
target_compile_options(stone-shell-integration-tests PRIVATE /utf-8)
set_target_properties(stone-shell-integration-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/subsystem/$<CONFIG>")
add_test(NAME stone-shell-launch-registration COMMAND stone-shell-integration-tests --launch-registration "${STONE_TEST_ROOT}/shell-launch/$<CONFIG>")
set_tests_properties(stone-shell-launch-registration PROPERTIES LABELS "host;shell;unit" TIMEOUT 30)
add_test(NAME stone-shell-notification-registration COMMAND stone-shell-integration-tests --notification-registration "${STONE_TEST_ROOT}/shell-notification/$<CONFIG>")
set_tests_properties(stone-shell-notification-registration PROPERTIES LABELS "host;shell;unit" TIMEOUT 30)


stone_test_context()

set_property(TEST stone-shell-icons APPEND PROPERTY LABELS unit)
