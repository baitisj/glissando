add_library(gliss_rig_control STATIC
    HamlibRigController.cpp
    SerialPortRigController.cpp
    SerialPortInRigController.cpp
    SerialPortOutRigController.cpp
)

target_include_directories(gliss_rig_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)

if(HAMLIB_ADD_DEPENDENCY)
add_dependencies(gliss_rig_control hamlib)
endif(HAMLIB_ADD_DEPENDENCY)

if(WIN32)
# Add OmniRig support on Windows
add_subdirectory(omnirig)
target_link_libraries(gliss_rig_control PUBLIC gliss_rig_control_omnirig)
endif(WIN32)

target_link_libraries(gliss_rig_control PUBLIC gliss_util)
target_compile_options(gliss_rig_control PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})

if(UNITTEST)
    add_executable(glissando_drive_servo_test test/DriveServoTest.cpp)
    target_compile_options(glissando_drive_servo_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME glissando_drive_servo COMMAND glissando_drive_servo_test)
    add_executable(glissando_smoke_gauge_test test/SmokeGaugeTest.cpp)
    target_compile_options(glissando_smoke_gauge_test PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
    add_test(NAME glissando_smoke_gauge COMMAND glissando_smoke_gauge_test)
endif(UNITTEST)
