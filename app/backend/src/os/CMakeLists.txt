if(APPLE)
    set(OS_WRAP_FILES osx_interface.mm)
elseif(WIN32)
    set(OS_WRAP_FILES windows_interface.cpp)
else()
    set(OS_WRAP_FILES osx_stubs.cpp)
endif()

add_library(gliss_os_wrapper STATIC
    ${OS_WRAP_FILES}
)

if(APPLE)
target_link_libraries(gliss_os_wrapper "-framework AppKit -framework CoreGraphics -framework CoreFoundation -framework AVFoundation")
endif(APPLE)

target_compile_options(gliss_os_wrapper PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_include_directories(gliss_os_wrapper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)
