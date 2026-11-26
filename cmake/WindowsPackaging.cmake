if(NOT STONE_BUILD_SETTINGS)
    message(FATAL_ERROR "Stone packages require STONE_BUILD_SETTINGS=ON.")
endif()

set(STONE_PACKAGE_PUBLISHER "CN=Trebledot" CACHE STRING "MSIX signing certificate subject")
set(STONE_PACKAGE_NAME "Trebledot.Stone" CACHE STRING "Stable MSIX identity name")
set(STONE_PACKAGE_BUNDLE_ARCHITECTURES "x64;ARM64" CACHE STRING "Architectures required by StoneMSIXBundle")
option(STONE_PACKAGE_INSTALL_SERVICE "Install the Stone Windows SCM service" ON)
if(NOT STONE_PACKAGE_INSTALL_SERVICE)
    message(FATAL_ERROR "Stone packages require STONE_PACKAGE_INSTALL_SERVICE=ON. See docs/windows/packaging.md.")
endif()
if(NOT STONE_PACKAGE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.0$")
    message(FATAL_ERROR "STONE_PACKAGE_VERSION must be major.minor.build.0; MSI ignores a fourth version field.")
endif()

find_program(STONE_PACKAGE_POWERSHELL NAMES pwsh REQUIRED)
find_program(STONE_WIX_EXECUTABLE NAMES wix)
find_program(STONE_MAKEAPPX_EXECUTABLE NAMES makeappx
    HINTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
find_program(STONE_SIGNTOOL_EXECUTABLE NAMES signtool
    HINTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
find_program(STONE_MAKEPRI_EXECUTABLE NAMES makepri
    HINTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
# Signing is optional for local inspection. No private keys/passwords in CMake.
set(STONE_PACKAGE_CERTIFICATE_THUMBPRINT "" CACHE STRING "CurrentUser/My signing certificate thumbprint")
set(STONE_PACKAGE_TIMESTAMP_URL "" CACHE STRING "RFC 3161 timestamp server URL")

# Native executables use the MSVC runtime. Copy its redistributable DLLs rather
# than assuming a VC redistributable is installed on the destination computer.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
set(package_crt "")
foreach(runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    string(APPEND package_crt "    \"${runtime}\",\n")
endforeach()
string(REGEX REPLACE ",\n$" "\n" package_crt "${package_crt}")
set(package_directory "${stone_platform_bin}/$<CONFIG>/packages")
set(package_config "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/package.json")
set(package_arch "${STONE_WINDOWS_ARCH}")
string(TOLOWER "${package_arch}" package_arch)
foreach(tool STONE_WIX_EXECUTABLE STONE_MAKEAPPX_EXECUTABLE STONE_SIGNTOOL_EXECUTABLE STONE_MAKEPRI_EXECUTABLE)
    if(NOT ${tool})
        set(${tool} "")
    endif()
endforeach()
set(package_bundle_inputs "")
foreach(arch IN LISTS STONE_PACKAGE_BUNDLE_ARCHITECTURES)
    if(NOT arch MATCHES "^(x64|ARM64)$")
        message(FATAL_ERROR "Bundle architecture must be x64 or ARM64: ${arch}")
    endif()
    string(TOLOWER "${arch}" msix_arch)
    string(APPEND package_bundle_inputs "    \"${PROJECT_SOURCE_DIR}/bin/${arch}/$<CONFIG>/packages/Stone_${STONE_PACKAGE_VERSION}_${msix_arch}.msix\",\n")
endforeach()
string(REGEX REPLACE ",\n$" "\n" package_bundle_inputs "${package_bundle_inputs}")
# Certificate subjects may contain quotes; explicit tool paths may use Windows
# separators. Escape JSON independently from XML and command-line quoting.
foreach(value IN ITEMS package_arch STONE_PACKAGE_VERSION STONE_PACKAGE_NAME STONE_PACKAGE_PUBLISHER
        stone_platform_bin package_directory PROJECT_SOURCE_DIR PROJECT_BINARY_DIR CMAKE_VS_MSBUILD_COMMAND
        STONE_WIX_EXECUTABLE STONE_MAKEAPPX_EXECUTABLE STONE_MAKEPRI_EXECUTABLE STONE_SIGNTOOL_EXECUTABLE
        STONE_PACKAGE_CERTIFICATE_THUMBPRINT STONE_PACKAGE_TIMESTAMP_URL)
    string(REPLACE "\\" "\\\\" escaped "${${value}}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    string(REPLACE "\t" "\\t" escaped "${escaped}")
    set(${value}_json "${escaped}")
endforeach()
configure_file("${PROJECT_SOURCE_DIR}/cmake/WindowsPackageConfig.json.in" "${CMAKE_CURRENT_BINARY_DIR}/package.json.in" @ONLY)
file(GENERATE OUTPUT "${package_config}" INPUT "${CMAKE_CURRENT_BINARY_DIR}/package.json.in")
