add_library(fdv_text_messaging STATIC
    Data2GLink.cpp
    Data2GTransport.cpp
    FrameAnnotation.cpp
    FrameCodec.cpp
    HeardStationList.cpp
    KeyingPlan.cpp
    MessageStore.cpp
    SnoopFeed.cpp
    TextMessagingProtocol.cpp
    TextMessagingSession.cpp
)

target_include_directories(fdv_text_messaging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(fdv_text_messaging PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_text_messaging PUBLIC sqlite3 Threads::Threads)

# GCC 14 at -O3 reports -Wfree-nonheap-object inside vector::_M_realloc_append
# for kissEncode()'s reserve-then-push_back on a local vector (GCC PR 115016,
# fixed in libstdc++ trunk only). It stays a warning, for this file alone, so
# a real one anywhere else still fails the build. Drop this once the GCCs we
# build with no longer raise it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(Data2GLink.cpp PROPERTIES
        COMPILE_OPTIONS -Wno-error=free-nonheap-object)
endif()

if(UNITTEST)
    add_executable(fdv_text_messaging_test test/FrameCodecTest.cpp)
    target_link_libraries(fdv_text_messaging_test fdv_text_messaging)
    target_compile_options(fdv_text_messaging_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_frame_codec COMMAND fdv_text_messaging_test)

    add_executable(fdv_text_messaging_store_test test/MessageStoreTest.cpp)
    target_link_libraries(fdv_text_messaging_store_test fdv_text_messaging)
    target_compile_options(fdv_text_messaging_store_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_store COMMAND fdv_text_messaging_store_test)

    add_executable(fdv_text_messaging_protocol_test test/TextMessagingProtocolTest.cpp)
    target_link_libraries(fdv_text_messaging_protocol_test fdv_text_messaging)
    target_compile_options(fdv_text_messaging_protocol_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_protocol COMMAND fdv_text_messaging_protocol_test)

    add_executable(fdv_text_messaging_us_data_segments_test test/UsDataSegmentsTest.cpp)
    target_link_libraries(fdv_text_messaging_us_data_segments_test fdv_text_messaging)
    target_compile_options(fdv_text_messaging_us_data_segments_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_us_data_segments COMMAND fdv_text_messaging_us_data_segments_test)

    add_executable(fdv_text_messaging_data2g_test test/Data2GTest.cpp)
    target_link_libraries(fdv_text_messaging_data2g_test fdv_text_messaging)
    target_compile_options(fdv_text_messaging_data2g_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_data2g COMMAND fdv_text_messaging_data2g_test)

    add_executable(fdv_text_messaging_keying_plan_test test/KeyingPlanTest.cpp)
    target_link_libraries(fdv_text_messaging_keying_plan_test fdv_text_messaging)
    target_compile_options(fdv_text_messaging_keying_plan_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_keying_plan COMMAND fdv_text_messaging_keying_plan_test)

    add_executable(fdv_text_messaging_frame_annotation_test test/FrameAnnotationTest.cpp)
    target_link_libraries(fdv_text_messaging_frame_annotation_test fdv_text_messaging)
    target_compile_options(fdv_text_messaging_frame_annotation_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_frame_annotation COMMAND fdv_text_messaging_frame_annotation_test)

    add_executable(fdv_text_messaging_snoop_test test/SnoopFeedTest.cpp)
    target_link_libraries(fdv_text_messaging_snoop_test fdv_text_messaging)
    target_compile_options(fdv_text_messaging_snoop_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_snoop COMMAND fdv_text_messaging_snoop_test)
endif(UNITTEST)
