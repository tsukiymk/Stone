add_executable(stone-image-classpath-tests image-tools-tests.cpp)
target_link_libraries(stone-image-classpath-tests PRIVATE stone-image-core)
target_compile_options(stone-image-classpath-tests PRIVATE /W4 /utf-8)
set_target_properties(stone-image-classpath-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${STONE_TEST_ROOT}/image-tools/$<CONFIG>" FOLDER "Tests/Image")
add_dependencies(stone-image-classpath-tests StoneImageTool)
add_test(NAME stone-image-classpaths COMMAND stone-image-classpath-tests
    "${STONE_TEST_ROOT}/image-tools/run" "$<TARGET_FILE:StoneImageTool>")
set_tests_properties(stone-image-classpaths PROPERTIES LABELS "image;unit" TIMEOUT 120)
stone_test_context()
