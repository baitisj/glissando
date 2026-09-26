add_library(fdv_audio_pipeline STATIC
    AgcStep.cpp
    AudioPipeline.cpp
    EitherOrStep.cpp
    IPipelineStep.cpp
    LevelAdjustStep.cpp
    LinkStep.cpp
    MinimalRealTimeHelper.cpp
    MuteStep.cpp
    ResampleStep.cpp
    RNNoiseStep.cpp
    TapStep.cpp
    ToneInterfererStep.cpp
)

target_compile_options(fdv_audio_pipeline PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_audio_pipeline PRIVATE ${LIBEBUR128} agc rnnoise fdv_os_wrapper samplerate fdv_util)
target_link_libraries(fdv_audio_pipeline INTERFACE rnnoise)
target_include_directories(fdv_audio_pipeline PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)
target_include_directories(fdv_audio_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(BUILD_BACKEND_UNITTESTS)
macro(DefineUnitTest utName)
    add_executable(${utName} test/${utName}.cpp)
    target_link_libraries(${utName} PRIVATE fdv_audio_pipeline fdv_util samplerate)
    target_include_directories(${utName} PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)

    add_test(NAME pipeline_${utName} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${utName})
endmacro()

DefineUnitTest(AgcStepTest)
DefineUnitTest(AudioPipelineTest)
DefineUnitTest(EitherOrTest)
DefineUnitTest(LevelAdjustTest)
DefineUnitTest(ResampleTest)
DefineUnitTest(TapTest)

endif(BUILD_BACKEND_UNITTESTS)
