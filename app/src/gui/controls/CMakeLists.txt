add_library(gliss_gui_controls STATIC
    plot.cpp
    plot_scalar.cpp
)

target_include_directories(gliss_gui_controls PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/../..)
add_dependencies(gliss_gui_controls codec2)
target_compile_options(gliss_gui_controls PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(gliss_gui_controls PUBLIC gliss_util)

if(BOOTSTRAP_WXWIDGETS)
    add_dependencies(gliss_gui_controls wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid wx::xrc)
    target_compile_definitions(gliss_gui_controls PRIVATE ${WXBUILD_BUILD_DEFS})
    target_include_directories(gliss_gui_controls PRIVATE ${WXBUILD_INCLUDES})
endif(BOOTSTRAP_WXWIDGETS)

