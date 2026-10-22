add_library(gliss_gui_util STATIC
    LabelOverrideAccessible.cpp
    NameOverrideAccessible.cpp
    wxListViewComboPopup.cpp
    WindowPositionRestore.cpp)

target_compile_options(gliss_gui_util PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(gliss_gui_util PUBLIC gliss_util)

if(BOOTSTRAP_WXWIDGETS)
    add_dependencies(gliss_gui_util wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid wx::xrc)
    target_compile_definitions(gliss_gui_util PRIVATE ${WXBUILD_BUILD_DEFS})
    target_include_directories(gliss_gui_util PRIVATE ${WXBUILD_INCLUDES})
endif(BOOTSTRAP_WXWIDGETS)
