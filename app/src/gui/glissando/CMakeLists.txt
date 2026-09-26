add_library(fdv_gui_glissando STATIC
    ChaoticaControls.cpp
    ChaoticaTheme.cpp
    GlissandoConsole.cpp
    GlissandoScope.cpp)

target_include_directories(fdv_gui_glissando PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_compile_options(fdv_gui_glissando PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_gui_glissando PUBLIC glissando_modem)

if(BOOTSTRAP_WXWIDGETS)
    add_dependencies(fdv_gui_glissando wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid wx::xrc)
    target_compile_definitions(fdv_gui_glissando PRIVATE ${WXBUILD_BUILD_DEFS})
    target_include_directories(fdv_gui_glissando PRIVATE ${WXBUILD_INCLUDES})
endif(BOOTSTRAP_WXWIDGETS)
