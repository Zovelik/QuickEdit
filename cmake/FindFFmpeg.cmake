# Finds the FFmpeg libraries and defines one imported target per component:
#   FFmpeg::avformat, FFmpeg::avcodec, FFmpeg::avutil, FFmpeg::avfilter,
#   FFmpeg::avdevice, FFmpeg::swresample, FFmpeg::swscale
#
# Windows: prebuilt shared LGPL build, from FFMPEG_ROOT or by default
#   3rdparty/ffmpeg/win64 (https://github.com/BtbN/FFmpeg-Builds, the
#   "win64-lgpl-shared" archive). FFMPEG_RUNTIME_DLLS lists the DLLs to copy
#   next to the executable.
# Linux: the system libraries, through pkg-config.
#
# FFmpeg 7.1 or newer is required (Vulkan hardware decoding and encoding).

set(FFMPEG_COMPONENTS avformat avcodec avutil avfilter avdevice swresample swscale)

if (WIN32)
    if (NOT FFMPEG_ROOT)
        set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/win64")
    endif()

    set(FFMPEG_RUNTIME_DLLS "")
    set(FFmpeg_FOUND TRUE)

    foreach(component ${FFMPEG_COMPONENTS})
        find_library(FFMPEG_${component}_LIBRARY NAMES ${component} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
        file(GLOB dll "${FFMPEG_ROOT}/bin/${component}-*.dll")

        if (NOT FFMPEG_${component}_LIBRARY OR NOT dll)
            set(FFmpeg_FOUND FALSE)
            continue()
        endif()

        add_library(FFmpeg::${component} SHARED IMPORTED)
        set_target_properties(FFmpeg::${component} PROPERTIES
            IMPORTED_IMPLIB "${FFMPEG_${component}_LIBRARY}"
            IMPORTED_LOCATION "${dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
        )

        list(APPEND FFMPEG_RUNTIME_DLLS "${dll}")
    endforeach()
else()
    find_package(PkgConfig REQUIRED)
    set(FFmpeg_FOUND TRUE)

    foreach(component ${FFMPEG_COMPONENTS})
        pkg_check_modules(PC_${component} IMPORTED_TARGET lib${component})

        if (NOT PC_${component}_FOUND)
            set(FFmpeg_FOUND FALSE)
            continue()
        endif()

        add_library(FFmpeg::${component} INTERFACE IMPORTED)
        target_link_libraries(FFmpeg::${component} INTERFACE PkgConfig::PC_${component})
    endforeach()
endif()

if (NOT FFmpeg_FOUND AND FFmpeg_FIND_REQUIRED)
    message(FATAL_ERROR
        "FFmpeg not found. On Windows, extract the 'win64-lgpl-shared' build of "
        "https://github.com/BtbN/FFmpeg-Builds into 3rdparty/ffmpeg/win64 or set "
        "FFMPEG_ROOT. On Linux, install the FFmpeg development packages.")
endif()
