set(WARNINGS_AS_ERRORS_FLAGS -Wall -Wextra -Werror -Wno-null-pointer-subtraction)

set(FREEDV_SOURCES
    main.cpp
    topFrame.cpp
    util.cpp
    playrec.cpp
    ongui.cpp
    freedv_interface.cpp
    glissando_host.cpp
)

set(FREEDV_LINK_LIBS_OSX
    "-framework AVFoundation"
)

# Configure logging
add_definitions(-DULOG_HAVE_TIME)
add_definitions(-DULOG_NO_COLOR)

# Include generated headers
include_directories(${CMAKE_BINARY_DIR}/generated)

# Compile FreeDV components
add_subdirectory(audio)
add_subdirectory(config)
# The Glissando modem lives at the top of the repository, beside the Python
# prototype its tests are checked against.
add_subdirectory(${CMAKE_SOURCE_DIR}/../modem ${CMAKE_BINARY_DIR}/modem)
add_subdirectory(gui)
add_subdirectory(pipeline)
add_subdirectory(rig_control)
add_subdirectory(text_messaging)

# WIN32 is needed for Windows GUI apps and is ignored for UNIX like systems.
add_executable(freedv WIN32 ${FREEDV_SOURCES})

# The target keeps FreeDV's name, so everything that builds on it still
# works; the program it makes is glissando (glissando.exe on Windows).
set_target_properties(freedv PROPERTIES OUTPUT_NAME glissando)

target_compile_options(freedv PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(freedv fdv_audio fdv_audio_pipeline fdv_audio_pipeline_extra fdv_config fdv_gui_controls fdv_gui_dialogs fdv_gui_glissando fdv_gui_util fdv_os_wrapper fdv_rig_control fdv_reporting fdv_text_messaging fdv_util codec2 git_version)

# Add dependencies for other statically built libraries, if applicable.
if(FREEDV_STATIC_DEPS)
    add_dependencies(freedv ${FREEDV_STATIC_DEPS})
    add_dependencies(fdv_audio ${FREEDV_STATIC_DEPS})
    add_dependencies(fdv_audio_pipeline_extra ${FREEDV_STATIC_DEPS})
endif(FREEDV_STATIC_DEPS)

# Link other dependencies
if(APPLE)
target_link_libraries(freedv ${FREEDV_LINK_LIBS} ${FREEDV_LINK_LIBS_OSX})
elseif(WIN32)
target_link_libraries(freedv ${FREEDV_LINK_LIBS} ws2_32)
else()
target_link_libraries(freedv ${FREEDV_LINK_LIBS})
endif(APPLE)

if(BOOTSTRAP_WXWIDGETS)
    target_compile_definitions(freedv PRIVATE ${WXBUILD_BUILD_DEFS})
    target_include_directories(freedv PRIVATE ${WXBUILD_INCLUDES})
    
    add_dependencies(freedv wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid wx::xrc)
    target_link_libraries(freedv wx::core wx::base wx::aui wx::html wx::net wx::adv wx::propgrid)
endif(BOOTSTRAP_WXWIDGETS)

# For older Xcode (< 9.0), bypass usage of @available.
if(APPLE)
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0.0.900037)
add_definitions(-DAPPLE_OLD_XCODE)
endif(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0.0.900037)
endif(APPLE)

# If on Linux, force static libstdc++. This is to prevent versioning
# problems when using AppImages.
if(LINUX AND STATIC_LIBGCC)
    target_link_options(freedv PRIVATE -static-libgcc -static-libstdc++)
endif(LINUX AND STATIC_LIBGCC)

# On Linux/GTK builds, make the GTK3 headers directly accessible in main.cpp
# for the button-flash suppression code.
if(LINUX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GTK3 QUIET gtk+-3.0)
        if(GTK3_FOUND)
            # gtk+-3.0.pc being present only means GTK3 *headers* exist
            # somewhere on the system -- it says nothing about whether the
            # wxWidgets we're linking against was itself built against GTK3.
            # wx/defs.h unconditionally forward-declares a stand-in
            # `GdkWindow` typedef whenever __WXGTK__ is set, and picks
            # between the real GTK3 layout (struct _GdkWindow) or the
            # legacy GTK2 one (struct _GdkDrawable) based on whether
            # __WXGTK3__ is *also* defined by wx's own (toolkit-specific)
            # setup.h. If wx was actually built for GTK2 -- e.g. a system
            # with both gtk2-devel and gtk3-devel installed, where wx-config
            # resolves to a GTK2 build -- directly #include-ing <gtk/gtk.h>
            # alongside wx headers gets two conflicting GdkWindow
            # declarations in the same translation unit, regardless of
            # include order. Probe for __WXGTK3__ using wx's own headers
            # before trusting the independent pkg-config detection.
            include(CheckCXXSymbolExists)
            set(CMAKE_REQUIRED_INCLUDES ${wxWidgets_INCLUDE_DIRS})
            # wxWidgets_DEFINITIONS is a list of bare macro tokens (correct
            # for the COMPILE_DEFINITIONS property UsewxWidgets.cmake sets
            # it via) -- but CMAKE_REQUIRED_DEFINITIONS needs full "-DFOO"
            # strings. Without the prefix, check_cxx_symbol_exists's probe
            # compile passes each bare token as a raw command-line argument,
            # which the compiler treats as a (nonexistent) input filename
            # and errors out on -- silently failing the whole check closed
            # (WX_BUILT_FOR_GTK3 always false) even on a real GTK3 wx build.
            set(WX_GTK3_PROBE_DEFINITIONS ${wxWidgets_DEFINITIONS})
            list(TRANSFORM WX_GTK3_PROBE_DEFINITIONS PREPEND "-D")
            set(CMAKE_REQUIRED_DEFINITIONS ${WX_GTK3_PROBE_DEFINITIONS})
            set(CMAKE_REQUIRED_FLAGS "${wxWidgets_CXX_FLAGS}")
            check_cxx_symbol_exists(__WXGTK3__ "wx/defs.h" WX_BUILT_FOR_GTK3)
            unset(CMAKE_REQUIRED_INCLUDES)
            unset(CMAKE_REQUIRED_DEFINITIONS)
            unset(CMAKE_REQUIRED_FLAGS)

            if(WX_BUILT_FOR_GTK3)
                message(STATUS "GTK3 development files found, will implement Linux-specific GUI workarounds")
                target_include_directories(freedv PRIVATE ${GTK3_INCLUDE_DIRS})
                target_link_libraries(freedv ${GTK3_LIBRARIES})
                add_definitions(-DHAS_GTK3)

                # fdv_gui_util (added via add_subdirectory(gui) above, before this
                # point) needs these too -- WindowPositionRestore.cpp lives there,
                # not in freedv's own sources. add_definitions()/directory-scoped
                # include dirs only propagate to subdirectories added *after* this
                # point, so it has to be set on the already-created target directly.
                target_compile_definitions(fdv_gui_util PRIVATE HAS_GTK3)
                target_include_directories(fdv_gui_util PRIVATE ${GTK3_INCLUDE_DIRS})
            else(WX_BUILT_FOR_GTK3)
                message(WARNING "GTK3 development files (gtk+-3.0.pc) were found, but the detected wxWidgets does not define __WXGTK3__ (it appears to target GTK2) -- including <gtk/gtk.h> directly alongside wx headers would conflict, so Linux-specific GUI workarounds (including window position restore) will be disabled")
            endif(WX_BUILT_FOR_GTK3)
        else(GTK3_FOUND)
            message(WARNING "GTK3 development files (gtk+-3.0.pc, e.g. package libgtk-3-dev) not found -- Linux-specific GUI workarounds (including window position restore) will be disabled")
        endif(GTK3_FOUND)
    else(PkgConfig_FOUND)
        message(WARNING "pkg-config not found -- cannot detect GTK3 development files, Linux-specific GUI workarounds (including window position restore) will be disabled")
    endif(PkgConfig_FOUND)
endif(LINUX)

# If a release tag's set, pass it to the compiler.
if(FREEDV_VERSION_TAG)
add_definitions(-DFREEDV_VERSION_TAG="${FREEDV_VERSION_TAG}")
endif(FREEDV_VERSION_TAG)

# Insert source and generated header directories before other search directories.
include_directories(BEFORE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS freedv
    RUNTIME DESTINATION bin
)

