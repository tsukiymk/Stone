# Full desktop payload and package identity, shared by the installer and bundle.
# Stone has no privileged MSI bootstrap service; this MSIX runs the payload directly.
add_custom_target(msixgluepackage
    COMMAND "${STONE_PACKAGE_POWERSHELL}" -NoProfile -File "${PROJECT_SOURCE_DIR}/tools/packaging/BuildPackages.ps1"
        -ConfigFile "${package_config}" -Format MSIX
    DEPENDS StoneService StoneSettings stone-art-runtime stone-image-tools
    USES_TERMINAL VERBATIM)
set_target_properties(msixgluepackage PROPERTIES FOLDER Packaging)
