# The Glissando melodic chirp modem. No wxWidgets or codec2 dependency, so it
# also builds on its own for quick iteration:
#   cmake -S modem -B build-modem -DUNITTEST=ON
#   cmake --build build-modem && ctest --test-dir build-modem
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.13)
    project(glissando_modem CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    option(UNITTEST "Build unittest binaries." OFF)
    if(UNITTEST)
        enable_testing()
    endif(UNITTEST)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif()
    set(WARNINGS_AS_ERRORS_FLAGS -Wall -Wextra -Werror)
    find_package(Threads REQUIRED)
endif()

add_library(glissando_modem STATIC
    GlissandoChord.cpp
    GlissandoDemod.cpp
    GlissandoFec.cpp
    GlissandoFft.cpp
    GlissandoLink.cpp
    GlissandoModem.cpp
    GlissandoReceiver.cpp
)

target_include_directories(glissando_modem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(glissando_modem PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(glissando_modem PUBLIC Threads::Threads)

if(UNITTEST)
    macro(glissando_test name source)
        add_executable(${name} ${source} test/GlissandoTestUtil.h)
        target_link_libraries(${name} glissando_modem)
        target_compile_options(${name} PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
        add_test(NAME ${name} COMMAND ${name} ${ARGN})
    endmacro()

    glissando_test(glissando_fec_test test/GlissandoFecTest.cpp)
    glissando_test(glissando_modulate_test test/GlissandoModulateTest.cpp)
    glissando_test(glissando_decode_test test/GlissandoDecodeTest.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/test/python_g4_clip.raw)
    glissando_test(glissando_receiver_test test/GlissandoReceiverTest.cpp)
    glissando_test(glissando_link_test test/GlissandoLinkTest.cpp)
    glissando_test(glissando_timing_test test/GlissandoTimingTest.cpp)
    glissando_test(glissando_chord_test test/GlissandoChordTest.cpp)
endif(UNITTEST)
