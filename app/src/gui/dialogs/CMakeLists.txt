add_library(gliss_gui_dialogs STATIC
    dlg_audiooptions.cpp
    dlg_easy_setup.cpp
    dlg_options.cpp
    dlg_ptt.cpp
    dlg_snoop.cpp
    dlg_text_messaging.cpp
    monitor_volume_adj.cpp
    tot_warning.cpp)

target_include_directories(gliss_gui_dialogs PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/../..)

target_link_libraries(gliss_gui_dialogs PUBLIC samplerate gliss_gui_controls gliss_gui_glissando gliss_audio_pipeline_extra gliss_text_messaging)

target_compile_options(gliss_gui_dialogs PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})

if(HAMLIB_ADD_DEPENDENCY)
add_dependencies(gliss_gui_dialogs hamlib)
endif(HAMLIB_ADD_DEPENDENCY)

if(BOOTSTRAP_WXWIDGETS)
    add_dependencies(gliss_gui_dialogs wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid wx::xrc)
    target_compile_definitions(gliss_gui_dialogs PRIVATE ${WXBUILD_BUILD_DEFS})
    target_include_directories(gliss_gui_dialogs PRIVATE ${WXBUILD_INCLUDES})
endif(BOOTSTRAP_WXWIDGETS)
