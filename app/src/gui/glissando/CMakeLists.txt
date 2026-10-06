add_library(fdv_gui_glissando STATIC
    ChaoticaControls.cpp
    ChaoticaTheme.cpp
    GlissandoConsole.cpp
    GlissandoScope.cpp
    Globe.cpp
    MapBall.cpp)

target_include_directories(fdv_gui_glissando PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_compile_options(fdv_gui_glissando PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_gui_glissando PUBLIC glissando_modem)

if(UNITTEST)
    add_executable(glissando_globe_test test/GlobeTest.cpp Globe.cpp)
    target_compile_options(glissando_globe_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME glissando_globe COMMAND glissando_globe_test)
endif(UNITTEST)

if(BOOTSTRAP_WXWIDGETS)
    add_dependencies(fdv_gui_glissando wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid wx::xrc)
    target_compile_definitions(fdv_gui_glissando PRIVATE ${WXBUILD_BUILD_DEFS})
    target_include_directories(fdv_gui_glissando PRIVATE ${WXBUILD_INCLUDES})
endif(BOOTSTRAP_WXWIDGETS)
