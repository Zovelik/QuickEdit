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

# Configuration. The version is the last git tag ("v1.2.0" gives 1.2.0).
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

string(REGEX REPLACE "^v" "" APP_VERSION "${GIT_TAG}")

if (NOT APP_VERSION)
    set(APP_VERSION "0.1.0")
endif()

set(APP_NAME_STR "QuickEdit")
set(APP_VERSION_STR "${APP_VERSION}")

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

# Packages (cpack): an installer and a zip on Windows, an AppImage on Linux.
set(CPACK_PACKAGE_NAME "QuickEdit")
set(CPACK_PACKAGE_VENDOR "Zovelik")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Video editor, simple to use yet powerful, rendered with Vulkan")
set(CPACK_PACKAGE_VERSION "${APP_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "QuickEdit")
set(CPACK_PACKAGE_EXECUTABLES "QuickEdit;QuickEdit")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_SOURCE_GENERATOR "")
set(CPACK_PACKAGE_FILE_NAME "QuickEdit-${APP_VERSION}-${CMAKE_SYSTEM_NAME}")

if (EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
endif()

if (WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    include(package/windows/Installer.cmake)
elseif (UNIX AND NOT APPLE)
    # Native AppImage generator of CPack (a recent CMake, 4.3 in the
    # workflow, with appimagetool and patchelf): the desktop entry, icon and AppStream data
    # are installed in the freedesktop layout, FFmpeg and shaderc in lib/
    # (src/CMakeLists.txt).
    set(CPACK_GENERATOR "AppImage")
    set(CPACK_PACKAGE_ICON "quickedit.png")
endif()

include(CPack)
