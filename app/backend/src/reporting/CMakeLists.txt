add_library(gliss_reporting STATIC
    pskreporter.cpp
    pskreporter.h
    CsvReporter.cpp
    CsvReporter.h
    UdpReporter.cpp
    UdpReporter.h
)

target_compile_options(gliss_reporting PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(gliss_reporting PRIVATE yyjson git_version)
target_include_directories(gliss_reporting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
