find_program(STONE_SETTINGS_TEST_DOTNET NAMES dotnet HINTS "$ENV{ProgramFiles}/dotnet" REQUIRED)
set(package_ui_output "${STONE_TEST_ROOT}/settings/package-ui")
add_custom_target(stone-settings-package-ui-tests
    COMMAND "${CMAKE_COMMAND}" -E env
        DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
        DOTNET_CLI_USE_MSBUILD_SERVER=0 DOTNET_GENERATE_ASPNET_CERTIFICATE=false DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=false
        "DOTNET_CLI_HOME=${package_ui_output}/dotnet-home"
        "TEMP=${STONE_TEST_ROOT}/temp" "TMP=${STONE_TEST_ROOT}/temp"
        "${STONE_SETTINGS_TEST_DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/package-ui-tests.csproj"
        --configuration Debug --ignore-failed-sources
        "-p:BaseIntermediateOutputPath=${package_ui_output}/obj/"
        "-p:OutputPath=${package_ui_output}/bin/" -p:AppendTargetFrameworkToOutputPath=false -p:UseSharedCompilation=false
    BYPRODUCTS "${package_ui_output}/bin/package-ui-tests.dll"
    VERBATIM)
set_target_properties(stone-settings-package-ui-tests PROPERTIES FOLDER "Tests/Settings")
add_test(NAME stone-settings-package-ui COMMAND "${STONE_SETTINGS_TEST_DOTNET}" "${package_ui_output}/bin/package-ui-tests.dll"
    "${PROJECT_SOURCE_DIR}/src/windows/settings/Resources" "${package_ui_output}/run")
set_tests_properties(stone-settings-package-ui PROPERTIES LABELS "settings;packages;unit" TIMEOUT 30)
set(download_test_output "${STONE_TEST_ROOT}/settings/image-download")
set(download_test_dll "${download_test_output}/bin/image-download-tests.dll")
add_custom_target(stone-image-download-tests
    COMMAND "${CMAKE_COMMAND}" -E env
        DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
        DOTNET_GENERATE_ASPNET_CERTIFICATE=false DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=false
        "DOTNET_CLI_HOME=${download_test_output}/dotnet-home"
        "TEMP=${STONE_TEST_ROOT}/temp" "TMP=${STONE_TEST_ROOT}/temp"
        "${STONE_SETTINGS_TEST_DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/image-download-tests.csproj"
        --configuration Debug --ignore-failed-sources
        "-p:BaseIntermediateOutputPath=${download_test_output}/obj/"
        "-p:OutputPath=${download_test_output}/bin/"
        -p:AppendTargetFrameworkToOutputPath=false
    BYPRODUCTS "${download_test_dll}"
    VERBATIM)
set_target_properties(stone-image-download-tests PROPERTIES FOLDER "Tests/Settings")
add_test(NAME stone-image-download COMMAND "${STONE_SETTINGS_TEST_DOTNET}" "${download_test_dll}" "${download_test_output}/run")
set_tests_properties(stone-image-download PROPERTIES LABELS "settings;unit;download" TIMEOUT 60)

set(ui_test_output "${STONE_TEST_ROOT}/settings/ui-automation")
set(ui_test_dll "${ui_test_output}/bin/ui-automation-tests.dll")
add_custom_target(stone-settings-ui-tests
    COMMAND "${CMAKE_COMMAND}" -E env
        DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
        DOTNET_CLI_USE_MSBUILD_SERVER=0
        DOTNET_GENERATE_ASPNET_CERTIFICATE=false DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=false
        "DOTNET_CLI_HOME=${ui_test_output}/dotnet-home"
        "TEMP=${STONE_TEST_ROOT}/temp" "TMP=${STONE_TEST_ROOT}/temp"
        "${STONE_SETTINGS_TEST_DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/ui-automation-tests.csproj"
        --configuration Debug --ignore-failed-sources
        "-p:BaseIntermediateOutputPath=${ui_test_output}/obj/"
        "-p:OutputPath=${ui_test_output}/bin/"
        -p:AppendTargetFrameworkToOutputPath=false
        -p:UseSharedCompilation=false
    BYPRODUCTS "${ui_test_dll}"
    VERBATIM)
add_dependencies(stone-settings-ui-tests StoneSettings StoneClient stone-settings-package-ui-tests)
set_target_properties(stone-settings-ui-tests PROPERTIES FOLDER "Tests/Settings")
add_test(NAME stone-settings-ui-automation
    COMMAND "${STONE_SETTINGS_TEST_DOTNET}" "${ui_test_dll}"
        "${PROJECT_SOURCE_DIR}/bin/x64/Debug/settings/StoneSettings.exe" "${ui_test_output}/run"
        "${package_ui_output}/bin/package-ui-tests.exe"
    CONFIGURATIONS Debug)
set_tests_properties(stone-settings-ui-automation PROPERTIES
    LABELS "settings;ui;integration" TIMEOUT 240 RUN_SERIAL TRUE)
stone_test_context()
