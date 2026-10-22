add_library(gliss_config STATIC
    AudioConfiguration.cpp
    FreeDVConfiguration.cpp
    ReportingConfiguration.cpp
    RigControlConfiguration.cpp
    WxWidgetsConfigStore.cpp
)

target_include_directories(gliss_config PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)
target_compile_options(gliss_config PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(gliss_config PUBLIC gliss_util gliss_audio_pipeline_extra)

if(BOOTSTRAP_WXWIDGETS)
    add_dependencies(gliss_config wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid wx::xrc)
    target_compile_definitions(gliss_config PRIVATE ${WXBUILD_BUILD_DEFS})
    target_include_directories(gliss_config PRIVATE ${WXBUILD_INCLUDES})
endif(BOOTSTRAP_WXWIDGETS)
