# Makes the glissando target the macOS app bundle, Glissando.app, with its
# Finder and Dock icon drawn from contrib/glissando256x256.png. Included by
# src/CMakeLists.txt on Apple. Adapted from Tim Vetter's macOS port of
# Ragtime (github.com/baitisj/ragtime, packaging/macos/Bundle.cmake).
set(GLISSANDO_MACOS_DIR "${CMAKE_SOURCE_DIR}/macos")
set(GLISSANDO_MACOS_ICON "${CMAKE_CURRENT_BINARY_DIR}/generated/glissando.icns")
add_custom_command(OUTPUT "${GLISSANDO_MACOS_ICON}"
    COMMAND /bin/sh "${GLISSANDO_MACOS_DIR}/make-icon.sh"
        "${CMAKE_SOURCE_DIR}/contrib/glissando256x256.png" "${GLISSANDO_MACOS_ICON}"
    DEPENDS "${CMAKE_SOURCE_DIR}/contrib/glissando256x256.png" "${GLISSANDO_MACOS_DIR}/make-icon.sh"
    VERBATIM)
target_sources(glissando PRIVATE "${GLISSANDO_MACOS_ICON}")
set_source_files_properties("${GLISSANDO_MACOS_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_target_properties(glissando PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME Glissando
    MACOSX_BUNDLE_INFO_PLIST "${GLISSANDO_MACOS_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.baitisj.glissando"
    MACOSX_BUNDLE_BUNDLE_NAME Glissando
    MACOSX_BUNDLE_ICON_FILE glissando.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
