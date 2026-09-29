cmake_minimum_required(VERSION 3.13)

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24.
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
        cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")

set(PROJECT_NAME freedv-backend)
set(PROJECT_VERSION 1.0.0)
set(PROJECT_DESCRIPTION "Common backend code shared by FreeDV projects")
set(PROJECT_HOMEPAGE_URL "https://freedv.org")

# Makes FreeDV overridden CMake platform available for ARM MinGW builds.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/")

if(APPLE)
    project(
        ${PROJECT_NAME}
        VERSION ${PROJECT_VERSION}
        DESCRIPTION ${PROJECT_DESCRIPTION}
        HOMEPAGE_URL ${PROJECT_HOMEPAGE_URL}
        LANGUAGES C CXX OBJCXX
    )
else()
    project(
        ${PROJECT_NAME}
        VERSION ${PROJECT_VERSION}
        DESCRIPTION ${PROJECT_DESCRIPTION}
        HOMEPAGE_URL ${PROJECT_HOMEPAGE_URL}
        LANGUAGES C CXX
    )
endif(APPLE)

# Build universal ARM64 and x86_64 binaries on Mac.
if(BUILD_OSX_UNIVERSAL)
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")
endif(BUILD_OSX_UNIVERSAL)

# Mandate a minimum of C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include helpers
include(cmake/IsMacOSUniversalBinary.cmake)

# Samplerate Library
#   
if(NOT USE_STATIC_SAMPLERATE) 
    message(STATUS "Looking for samplerate...")
    find_library(LIBSAMPLERATE samplerate)
    find_path(LIBSAMPLERATE_INCLUDE_DIR samplerate.h)
    message(STATUS "  samplerate library: ${LIBSAMPLERATE}")
    message(STATUS "  samplerate headers: ${LIBSAMPLERATE_INCLUDE_DIR}")
    IsMacOSUniversalBinary("${LIBSAMPLERATE}" LIBSAMPLERATE_USABLE)
    if (NOT LIBSAMPLERATE_USABLE)
        message(STATUS "libsamplerate is not a universal binary, ignoring found library")
        set(LIBSAMPLERATE 0)
        set(LIBSAMPLERATE_INCLUDE_DIR "")
    endif()
    if(LIBSAMPLERATE AND LIBSAMPLERATE_INCLUDE_DIR)
        list(APPEND FREEDV_LINK_LIBS ${LIBSAMPLERATE})
        include_directories(${LIBSAMPLERATE_INCLUDE_DIR})
    else(LIBSTAMPLERATE AND LIBSAMPLERATE_INCLUDE_DIR)
        message(STATUS "Will attempt static build of samplerate.")
        include(cmake/BuildSamplerate.cmake)
    endif(LIBSAMPLERATE AND LIBSAMPLERATE_INCLUDE_DIR)
else(NOT USE_STATIC_SAMPLERATE)
    message(STATUS "Will attempt static build of samplerate.")
    include(cmake/BuildSamplerate.cmake)
endif(NOT USE_STATIC_SAMPLERATE)

# Enable CTest tests for freedv-gui
option(BUILD_BACKEND_UNITTESTS "Build unit test binaries." OFF)
if(BUILD_BACKEND_UNITTESTS)
message(STATUS "Enabling unit tests.")
enable_testing()
endif(BUILD_BACKEND_UNITTESTS)

add_subdirectory(src)
