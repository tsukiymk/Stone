add_custom_target(msipackage
    COMMAND "${STONE_PACKAGE_POWERSHELL}" -NoProfile -File "${PROJECT_SOURCE_DIR}/tools/packaging/BuildPackages.ps1"
        -ConfigFile "${package_config}" -Format MSI
    DEPENDS StoneService StoneSettings stone-art-runtime stone-image-tools
    USES_TERMINAL VERBATIM)
set_target_properties(msipackage PROPERTIES FOLDER Packaging)
add_custom_target(StoneMSI DEPENDS msipackage)
