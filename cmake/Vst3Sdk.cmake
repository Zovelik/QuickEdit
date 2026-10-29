# Hosting part of the VST3 SDK (MIT), from 3rdparty/vst3sdk: a clone of
# https://github.com/steinbergmedia/vst3sdk with its base, pluginterfaces
# and public.sdk submodules. Defines the vst3host library when present.

set(VST3_SDK_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/vst3sdk")

if (NOT EXISTS "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp")
    message(STATUS "VST3 SDK not found in ${VST3_SDK_ROOT}: no VST3 effects")
    return()
endif()

set(VST3_HOST_SOURCES
    ${VST3_SDK_ROOT}/pluginterfaces/base/conststringtable.cpp
    ${VST3_SDK_ROOT}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK_ROOT}/pluginterfaces/base/ustring.cpp

    ${VST3_SDK_ROOT}/base/source/baseiids.cpp
    ${VST3_SDK_ROOT}/base/source/fbuffer.cpp
    ${VST3_SDK_ROOT}/base/source/fdebug.cpp
    ${VST3_SDK_ROOT}/base/source/fobject.cpp
    ${VST3_SDK_ROOT}/base/source/fstreamer.cpp
    ${VST3_SDK_ROOT}/base/source/fstring.cpp
    ${VST3_SDK_ROOT}/base/source/updatehandler.cpp
    ${VST3_SDK_ROOT}/base/thread/source/fcondition.cpp
    ${VST3_SDK_ROOT}/base/thread/source/flock.cpp

    ${VST3_SDK_ROOT}/public.sdk/source/common/commoniids.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/common/memorystream.cpp

    ${VST3_SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/processdata.cpp
)

if (WIN32)
    list(APPEND VST3_HOST_SOURCES ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_win32.cpp)
elseif (UNIX AND NOT APPLE)
    list(APPEND VST3_HOST_SOURCES ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()

add_library(vst3host STATIC ${VST3_HOST_SOURCES})

# The SDK is written for C++17 (u8string() returning std::string...).
set_target_properties(vst3host PROPERTIES CXX_STANDARD 17)
target_include_directories(vst3host PUBLIC ${VST3_SDK_ROOT})
target_compile_definitions(vst3host PUBLIC $<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>)

if (MSVC)
    target_compile_options(vst3host PRIVATE /utf-8 /W0)
    target_compile_definitions(vst3host PUBLIC _UNICODE UNICODE NOMINMAX)
    target_link_libraries(vst3host PUBLIC ole32 shell32)
else()
    target_link_libraries(vst3host PUBLIC dl pthread stdc++fs)
endif()

set(VST3_FOUND TRUE)
