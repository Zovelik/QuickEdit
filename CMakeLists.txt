cmake_minimum_required(VERSION 3.15)

project(QuickEdit)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/plugins)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR})
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Configuration
set(APP_NAME_STR "QuickEdit")
if(GIT_TAG)
    set(APP_VERSION_STR "${GIT_TAG}")
else()
    set(APP_VERSION_STR "0.0.0")
endif()

set(APP_IDENTIFIER_STR "zovelik.quickedit")

configure_file(cmake/config.h.in config.h @ONLY)

# Dependencies configuration (must be set before 3rdparty is built)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# FFmpeg: prebuilt on Windows, system libraries on Linux
find_package(FFmpeg REQUIRED)

# VST3 SDK (hosting), optional
include(Vst3Sdk)

# Build
add_subdirectory(3rdparty)
add_subdirectory(tools)
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

# CPack
set(CPACK_PACKAGE_NAME "QuickEdit")
set(CPACK_PACKAGE_VENDOR "Zovelik")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_GENERATOR "ZIP")
set(CPACK_SOURCE_GENERATOR "")

execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_TAG)
    set(CPACK_PACKAGE_VERSION "${GIT_TAG}")
else()
    set(CPACK_PACKAGE_VERSION "0.0.0")
endif()

include(CPack)
