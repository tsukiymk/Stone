add_custom_target(msixinstallerpackage DEPENDS msixgluepackage)
add_custom_target(StoneMSIX DEPENDS msixinstallerpackage)
add_custom_target(bundle
    COMMAND "${STONE_PACKAGE_POWERSHELL}" -NoProfile -File "${PROJECT_SOURCE_DIR}/tools/packaging/BuildMsixBundle.ps1"
        -ConfigFile "${package_config}"
    DEPENDS msixinstallerpackage USES_TERMINAL VERBATIM)
add_custom_target(StoneMSIXBundle DEPENDS bundle)
set_target_properties(msixinstallerpackage bundle StoneMSIX StoneMSIXBundle PROPERTIES FOLDER Packaging)
