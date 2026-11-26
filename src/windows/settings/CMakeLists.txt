if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio ")
    message(FATAL_ERROR "StoneSettings requires a Visual Studio generator. Use -DSTONE_BUILD_SETTINGS=OFF for native-only builds.")
endif()

enable_language(CSharp)
include(CSharpUtilities)

set(STONE_SETTINGS_WINDOWS_APP_SDK_VERSION "2.5.1" CACHE STRING "Windows App SDK version for StoneSettings")
set(STONE_SETTINGS_WINDOWS_SDK_BUILDTOOLS_VERSION "10.0.26100.4654" CACHE STRING "Windows SDK build tools version for StoneSettings")
option(STONE_SETTINGS_AOT "Publish StoneSettings with Native AOT" ON)
option(STONE_SETTINGS_TRIM "Trim StoneSettings when publishing" ON)
if(STONE_SETTINGS_AOT AND NOT STONE_SETTINGS_TRIM)
    message(FATAL_ERROR "Native AOT requires trimming. Enable STONE_SETTINGS_TRIM or disable STONE_SETTINGS_AOT.")
endif()
set(settings_aot false)
set(settings_trim false)
if(STONE_SETTINGS_AOT)
    set(settings_aot true)
endif()
if(STONE_SETTINGS_TRIM)
    set(settings_trim true)
endif()

# Keep the C# target independent of the selected C++ Windows SDK. The .NET SDK
# supplies the corresponding managed Windows targeting pack through NuGet.
set(settings_framework "net10.0-windows10.0.26100.0")
set(settings_output_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/settings")
string(TOLOWER "${STONE_WINDOWS_ARCH}" settings_platform)
if(NOT settings_platform MATCHES "^(x64|arm64)$")
    message(FATAL_ERROR "StoneSettings supports x64 and ARM64. Detected: ${STONE_WINDOWS_ARCH}")
endif()

set(settings_sources
    App.xaml App.xaml.cs
    MainWindow.xaml MainWindow.xaml.cs
    WelcomeWindow.xaml WelcomeWindow.xaml.cs
    InstallWindow.xaml InstallWindow.xaml.cs
    ConfirmationWindow.xaml ConfirmationWindow.xaml.cs)
file(GLOB_RECURSE settings_views CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Controls/*.xaml"
    "${CMAKE_CURRENT_SOURCE_DIR}/Controls/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Views/*.xaml"
    "${CMAKE_CURRENT_SOURCE_DIR}/Views/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Models/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/ViewModels/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utils/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Helpers/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Services/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/Styles/*.xaml")
file(GLOB_RECURSE settings_assets CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*")
file(GLOB_RECURSE settings_resources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/*.resw")

add_executable(StoneSettings WIN32
    ${settings_sources}
    ${settings_views}
    ${settings_assets}
    ${settings_resources}
    app.manifest
    Package.appxmanifest)
csharp_set_xaml_cs_properties(${settings_sources} ${settings_views})
set_source_files_properties(App.xaml PROPERTIES VS_XAML_TYPE ApplicationDefinition)
# This target is an unpackaged desktop application, as in WSL's wslsettings.
# Keep the package manifest available in the IDE without enabling MSIX signing.
set_source_files_properties(Package.appxmanifest PROPERTIES VS_TOOL_OVERRIDE None)
set_source_files_properties(${settings_resources} PROPERTIES VS_TOOL_OVERRIDE PRIResource)
set_source_files_properties(${settings_assets} PROPERTIES
    VS_TOOL_OVERRIDE Content
    VS_COPY_TO_OUT_DIR PreserveNewest)

set_target_properties(StoneSettings PROPERTIES
    FOLDER windows
    DOTNET_SDK Microsoft.NET.Sdk
    DOTNET_TARGET_FRAMEWORK "${settings_framework}"
    RUNTIME_OUTPUT_DIRECTORY "${settings_output_directory}"
    VS_GLOBAL_RootNamespace StoneSettings
    VS_GLOBAL_Version "${STONE_VERSION_BASE}"
    VS_GLOBAL_AssemblyVersion "${STONE_VERSION_BASE}"
    VS_GLOBAL_FileVersion "${STONE_PACKAGE_VERSION}"
    VS_GLOBAL_InformationalVersion "${STONE_VERSION}"
    VS_GLOBAL_IncludeSourceRevisionInInformationalVersion false
    VS_GLOBAL_ApplicationIcon "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Stone.ico"
    VS_GLOBAL_DefaultLanguage en-US
    VS_GLOBAL_EnableDefaultItems false
    VS_GLOBAL_AppContainerApplication false
    VS_GLOBAL_AppxPackage false
    VS_GLOBAL_UseWinUI true
    VS_GLOBAL_WindowsPackageType None
    VS_GLOBAL_SelfContained true
    VS_GLOBAL_WindowsAppSDKSelfContained true
    VS_GLOBAL_ApplicationManifest "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
    VS_GLOBAL_TargetPlatformMinVersion "10.0.19041.0"
    VS_GLOBAL_SupportedOSPlatformVersion "10.0.19041.0"
    VS_GLOBAL_RuntimeIdentifier "win-${settings_platform}"
    VS_GLOBAL_RuntimeIdentifiers "win-${settings_platform}"
    VS_GLOBAL_PlatformTarget "${settings_platform}"
    VS_GLOBAL_AppendTargetFrameworkToOutputPath false
    VS_GLOBAL_AppendRuntimeIdentifierToOutputPath false
    VS_GLOBAL_ImplicitUsings enable
    VS_GLOBAL_Nullable enable
    VS_GLOBAL_LangVersion latest
    VS_GLOBAL_PublishAot "${settings_aot}"
    VS_GLOBAL_PublishTrimmed "${settings_trim}"
    VS_GLOBAL_TrimMode full
    VS_GLOBAL_NativeOutputPath "$(MSBuildProjectDirectory)/obj/native/$(Platform)/$(Configuration)/"
    VS_GLOBAL_CsWinRTAotOptimizerEnabled true
    VS_GLOBAL_CsWinRTAotWarningLevel 2
    VS_GLOBAL_AllowUnsafeBlocks true
    VS_GLOBAL_CscEnvironment "LIB="
    VS_GLOBAL_JsonSerializerIsReflectionEnabledByDefault false
    VS_GLOBAL_DirectoryBuildTargetsPath "${CMAKE_CURRENT_SOURCE_DIR}/Settings.targets"
    VS_GLOBAL_StoneSettingsWindowsAppSdkVersion "${STONE_SETTINGS_WINDOWS_APP_SDK_VERSION}"
    VS_GLOBAL_StoneThirdPartyNoticesPath "${PROJECT_SOURCE_DIR}/docs/third-party/THIRD_PARTY_NOTICES.txt"
    VS_PACKAGE_REFERENCES
        "Microsoft.WindowsAppSDK_${STONE_SETTINGS_WINDOWS_APP_SDK_VERSION};Microsoft.Windows.SDK.BuildTools_${STONE_SETTINGS_WINDOWS_SDK_BUILDTOOLS_VERSION}")

# Native AOT and trimming run at Publish, not Build. Keep this separate from the
# SDK-style C# target: CMake does not support custom build commands on that target.
add_custom_target(StoneSettingsPublish
    COMMAND "${CMAKE_COMMAND}" -E env --unset=LIB
        "${CMAKE_VS_MSBUILD_COMMAND}" "${CMAKE_CURRENT_BINARY_DIR}/StoneSettings.csproj"
        /restore /t:Publish "/p:Configuration=$<CONFIG>" "/p:Platform=${CMAKE_VS_PLATFORM_NAME}"
        /p:Optimize=true
        "/p:PublishDir=${settings_output_directory}/"
    DEPENDS StoneSettings
    USES_TERMINAL
    VERBATIM)
set_target_properties(StoneSettingsPublish PROPERTIES FOLDER windows)
