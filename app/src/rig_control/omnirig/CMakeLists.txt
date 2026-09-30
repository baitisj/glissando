add_library(fdv_rig_control_omnirig STATIC
    OmniRig_i.c
    OmniRigController.cpp
)

target_include_directories(fdv_rig_control_omnirig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_compile_options(fdv_rig_control_omnirig PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_rig_control_omnirig PUBLIC fdv_util)

# OmniRig_i.c is MIDL output. Its GUIDs are declared EXTERN_C and
# initialized in one go, which GCC (unlike Clang) warns about in C.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(OmniRig_i.c PROPERTIES
        COMPILE_OPTIONS -Wno-error)
endif()
