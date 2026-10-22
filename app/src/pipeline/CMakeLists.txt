add_library(gliss_audio_pipeline_extra STATIC
    ComputeRfSpectrumStep.cpp
    PlaybackStep.cpp
    RecordStep.cpp
    ResamplePlotStep.cpp
    TextMessagingModem.cpp
    TextMessagingReceiveStep.cpp
    TextMessagingTransport.cpp
    TextMessagingTxQueue.cpp
    TxRxThread.cpp
)

target_compile_options(gliss_audio_pipeline_extra PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(gliss_audio_pipeline_extra PUBLIC gliss_audio_pipeline gliss_text_messaging glissando_modem gliss_util samplerate)
target_include_directories(gliss_audio_pipeline_extra PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)

if(BOOTSTRAP_WXWIDGETS)
    add_dependencies(gliss_audio_pipeline_extra wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid wx::xrc)
    target_compile_definitions(gliss_audio_pipeline_extra PRIVATE ${WXBUILD_BUILD_DEFS})
    target_include_directories(gliss_audio_pipeline_extra PRIVATE ${WXBUILD_INCLUDES})
endif(BOOTSTRAP_WXWIDGETS)
