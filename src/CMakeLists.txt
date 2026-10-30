find_package(Vulkan REQUIRED)

# Shader compilation tool
function(compile_shader shader_source shader_output)
    set(src_input "${CMAKE_CURRENT_SOURCE_DIR}/${shader_source}")
    set(spirv "${CMAKE_CURRENT_BINARY_DIR}/${shader_source}.spv")
    set(output_full "${CMAKE_CURRENT_BINARY_DIR}/${shader_output}")

    get_filename_component(spirv_dir "${spirv}" DIRECTORY)
    get_filename_component(output_dir "${output_full}" DIRECTORY)

    add_custom_command(
        OUTPUT ${spirv}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${spirv_dir}
        COMMAND
            Vulkan::glslc ${src_input}
            --target-env=vulkan1.4
            -I${CMAKE_CURRENT_SOURCE_DIR}/shader
            -o ${spirv}
        DEPENDS ${src_input}
        COMMENT "Compiling ${shader_source}"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${output_full}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
        COMMAND spirv-to-header ${spirv} ${output_full}
        DEPENDS spirv-to-header ${spirv}
        COMMENT "Generating ${shader_output}"
        VERBATIM
    )
endfunction()

set(SHADERS
    shader/effect.vert
    shader/effect_blur.frag
    shader/effect_chromakey.frag
    shader/effect_color.frag
    shader/effect_pixelate.frag
    shader/effect_sharpen.frag
    shader/effect_vignette.frag
    shader/rgb_to_nv12.comp
    shader/layer.frag
    shader/layer.vert
    shader/text.frag
    shader/text.vert
    shader/yuv_to_rgb.comp
)

foreach(SHADER ${SHADERS})
    compile_shader(${SHADER} "${SHADER}.h")
    list(APPEND GENERATED_SHADER_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.h")
endforeach()

# Editing model, independent of the UI and of the GPU (see tests/).
add_library(QuickEditCore STATIC
    core/Animation.cpp
    core/Base64.cpp
    core/History.cpp
    core/Json.cpp
    core/Layers.cpp
    core/Paths.cpp
    core/Project.cpp
    core/Shaders.cpp
    core/Subtitles.cpp
    core/Text.cpp
    core/TextEdit.cpp
    core/VideoEffects.cpp
    core/ThreadPool.cpp
)

target_include_directories(QuickEditCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Media analysis and decoding (FFmpeg) and audio mixing, without UI.

add_library(QuickEditMedia STATIC
    audio/Loudness.cpp
    audio/Mixer.cpp

    media/HwDevice.cpp
    media/MediaFile.cpp
    media/MediaManager.cpp
    media/Thumbnails.cpp
    media/VideoPlayer.cpp
    media/VideoReader.cpp
    media/Waveform.cpp
)

target_link_libraries(QuickEditMedia PUBLIC QuickEditCore)
target_link_libraries(QuickEditMedia PUBLIC Vulkan::Vulkan)
target_link_libraries(QuickEditMedia PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swresample FFmpeg::swscale)

if (MSVC)
    # Sources are UTF-8
    target_compile_options(QuickEditCore PRIVATE /utf-8)
    target_compile_options(QuickEditMedia PRIVATE /utf-8)
endif()

set(QUICKEDIT_SRC
    main.cpp
    app/AppIcon.cpp
    app/EffectRack.cpp
    app/Exporter.cpp
    app/BorderlessWindow.cpp
    app/FileDrag.cpp
    app/GUIBase.cpp
    app/GUI.cpp
    app/I18n.cpp
    app/Inspector.cpp
    app/SubtitleGenerator.cpp
    app/KeyframeEditor.cpp
    app/ViewerTextEditor.cpp
    app/TimelineView.cpp

    audio/AudioEngine.cpp
    audio/Recorder.cpp
    audio/Vst3Plugin.cpp

    capture/ScreenRecorder.cpp

    render/Compositor.cpp
    render/FontLibrary.cpp
    render/Nv12Converter.cpp
    render/ShaderCompiler.cpp
    render/TextLayout.cpp
    render/VideoFrameRenderer.cpp

    vkutils/Buffer.cpp
    vkutils/CommandPool.cpp
    vkutils/ComputeShader.cpp
    vkutils/DescriptorPool.cpp
    vkutils/Device.cpp
    vkutils/Image.cpp
    vkutils/Instance.cpp
    vkutils/PipelineCache.cpp
    vkutils/Swapchain.cpp
    vkutils/util.cpp

    ${GENERATED_SHADER_HEADERS}
)

if (WIN32)
    # Icon of the executable (also used by SDL as the window icon)
    list(APPEND QUICKEDIT_SRC resources/app.rc)

    add_executable(QuickEdit WIN32 ${QUICKEDIT_SRC})
else()
    add_executable(QuickEdit ${QUICKEDIT_SRC})
endif()

target_include_directories(QuickEdit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/shader)
target_include_directories(QuickEdit PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_BINARY_DIR})
target_compile_definitions(QuickEdit PRIVATE UNICODE _UNICODE)

if (MSVC)
    # Sources are UTF-8
    target_compile_options(QuickEdit PRIVATE /utf-8)
endif()

target_link_libraries(QuickEdit PRIVATE QuickEditCore QuickEditMedia SDL3::SDL3 Vulkan::Vulkan imgui ImGuiColorTextEdit
    FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::avfilter FFmpeg::avdevice FFmpeg::swresample FFmpeg::swscale vst3host)

if (WIN32)
    # FFmpeg DLLs next to the executable
    add_custom_command(TARGET QuickEdit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} $<TARGET_FILE_DIR:QuickEdit>
        COMMAND_EXPAND_LISTS
    )

    install(FILES ${FFMPEG_RUNTIME_DLLS} DESTINATION .)
endif()

# Translations of the interface (app/I18n.h), next to the executable.
add_custom_command(TARGET QuickEdit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/lang $<TARGET_FILE_DIR:QuickEdit>/lang
)

# Speech recognition for the subtitles, when whisper.cpp was cloned (see
# 3rdparty/CMakeLists.txt).
if (QUICKEDIT_WHISPER)
    target_link_libraries(QuickEdit PRIVATE whisper)
    target_compile_definitions(QuickEdit PRIVATE QUICKEDIT_WHISPER)
endif()

# Compiler of the shaders of the user (render/ShaderCompiler.cpp): shaderc,
# from the Vulkan SDK on Windows (its DLL next to the executable).
if (WIN32)
    get_filename_component(VULKAN_SDK_LIB_DIR "${Vulkan_LIBRARY}" DIRECTORY)
    find_library(SHADERC_LIBRARY shaderc_shared HINTS "${VULKAN_SDK_LIB_DIR}" REQUIRED)
    find_file(SHADERC_DLL shaderc_shared.dll HINTS "${VULKAN_SDK_LIB_DIR}/../Bin" REQUIRED)

    target_link_libraries(QuickEdit PRIVATE ${SHADERC_LIBRARY})

    add_custom_command(TARGET QuickEdit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADERC_DLL} $<TARGET_FILE_DIR:QuickEdit>
    )

    install(FILES ${SHADERC_DLL} DESTINATION .)
else()
    find_library(SHADERC_LIBRARY NAMES shaderc_shared shaderc REQUIRED)
    target_link_libraries(QuickEdit PRIVATE ${SHADERC_LIBRARY})
endif()

if (WIN32)
    # SetWindowSubclass (app/BorderlessWindow.cpp), the windows to capture
    # and the formats of the cameras (capture/ScreenRecorder.cpp)
    target_link_libraries(QuickEdit PRIVATE comctl32 dwmapi strmiids ole32 oleaut32)
endif()

# Licenses of the libraries in the packages.
set(QUICKEDIT_LICENSE_DIR "licenses")

if (NOT WIN32)
    set(QUICKEDIT_LICENSE_DIR "${CMAKE_INSTALL_DATADIR}/quickedit/licenses")
endif()

foreach(license
        "SDL3|${CMAKE_SOURCE_DIR}/3rdparty/SDL/LICENSE.txt"
        "Dear ImGui|${CMAKE_SOURCE_DIR}/3rdparty/imgui/imgui/LICENSE.txt"
        "ImGuiColorTextEdit|${CMAKE_SOURCE_DIR}/3rdparty/ImGuiColorTextEdit/LICENSE"
        "VST3 SDK|${CMAKE_SOURCE_DIR}/3rdparty/vst3sdk/LICENSE.txt"
        "whisper.cpp|${CMAKE_SOURCE_DIR}/3rdparty/whisper.cpp/LICENSE"
        "FFmpeg|${FFMPEG_ROOT}/LICENSE.txt")
    string(REPLACE "|" ";" license "${license}")
    list(GET license 0 name)
    list(GET license 1 file)

    if (EXISTS "${file}")
        install(FILES "${file}" DESTINATION ${QUICKEDIT_LICENSE_DIR} RENAME "${name}.txt")
    endif()
endforeach()

# Installation: everything beside the executable on Windows; the
# freedesktop layout elsewhere (the translations in share/quickedit).
if (WIN32)
    install(TARGETS QuickEdit DESTINATION .)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/lang DESTINATION .)
    install(FILES ${CMAKE_SOURCE_DIR}/README.md DESTINATION .)
else()
    install(TARGETS QuickEdit DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/lang DESTINATION ${CMAKE_INSTALL_DATADIR}/quickedit)
    install(FILES ${CMAKE_SOURCE_DIR}/package/linux/quickedit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/package/linux/quickedit.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES ${CMAKE_SOURCE_DIR}/package/linux/quickedit-mime.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME quickedit.png)
endif()
