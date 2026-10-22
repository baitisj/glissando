add_library(gliss_text_messaging STATIC
    AnswerTempo.cpp
    FrameAnnotation.cpp
    FrameCodec.cpp
    HamText.cpp
    HeardStationList.cpp
    KeyingPlan.cpp
    MessageStore.cpp
    PartialCopies.cpp
    SnoopFeed.cpp
    TextMessagingProtocol.cpp
    TextMessagingSession.cpp
)

target_include_directories(gliss_text_messaging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(gliss_text_messaging PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(gliss_text_messaging PUBLIC sqlite3 Threads::Threads)
if(UNITTEST)
    add_executable(gliss_text_messaging_test test/FrameCodecTest.cpp)
    target_link_libraries(gliss_text_messaging_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_frame_codec COMMAND gliss_text_messaging_test)

    add_executable(gliss_text_messaging_store_test test/MessageStoreTest.cpp)
    target_link_libraries(gliss_text_messaging_store_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_store_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_store COMMAND gliss_text_messaging_store_test)

    add_executable(gliss_text_messaging_protocol_test test/TextMessagingProtocolTest.cpp)
    target_link_libraries(gliss_text_messaging_protocol_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_protocol_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_protocol COMMAND gliss_text_messaging_protocol_test)

    add_executable(gliss_text_messaging_us_data_segments_test test/UsDataSegmentsTest.cpp)
    target_link_libraries(gliss_text_messaging_us_data_segments_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_us_data_segments_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_us_data_segments COMMAND gliss_text_messaging_us_data_segments_test)

    add_executable(gliss_text_messaging_keying_plan_test test/KeyingPlanTest.cpp)
    target_link_libraries(gliss_text_messaging_keying_plan_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_keying_plan_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_keying_plan COMMAND gliss_text_messaging_keying_plan_test)

    add_executable(gliss_text_messaging_answer_tempo_test test/AnswerTempoTest.cpp)
    target_link_libraries(gliss_text_messaging_answer_tempo_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_answer_tempo_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_answer_tempo COMMAND gliss_text_messaging_answer_tempo_test)

    add_executable(gliss_text_messaging_frame_annotation_test test/FrameAnnotationTest.cpp)
    target_link_libraries(gliss_text_messaging_frame_annotation_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_frame_annotation_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_frame_annotation COMMAND gliss_text_messaging_frame_annotation_test)

    add_executable(gliss_text_messaging_partial_copies_test test/PartialCopiesTest.cpp)
    target_link_libraries(gliss_text_messaging_partial_copies_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_partial_copies_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_partial_copies COMMAND gliss_text_messaging_partial_copies_test)

    add_executable(gliss_text_messaging_snoop_test test/SnoopFeedTest.cpp)
    target_link_libraries(gliss_text_messaging_snoop_test gliss_text_messaging)
    target_compile_options(gliss_text_messaging_snoop_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME text_messaging_snoop COMMAND gliss_text_messaging_snoop_test)
endif(UNITTEST)
