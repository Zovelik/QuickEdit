add_subdirectory(SDL)
add_subdirectory(imgui)

# Code editor with syntax colors (shader editor), MIT.
add_library(ImGuiColorTextEdit STATIC ImGuiColorTextEdit/TextEditor.cpp)
target_include_directories(ImGuiColorTextEdit PUBLIC ImGuiColorTextEdit)
target_link_libraries(ImGuiColorTextEdit PUBLIC imgui)

# Speech recognition for the subtitles (whisper.cpp, MIT, cloned: see
# docs/ARCHITECTURE.md), static, on the GPU through Vulkan. Optional.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/whisper.cpp/CMakeLists.txt")
    set(QUICKEDIT_WHISPER ON PARENT_SCOPE)

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)

    add_subdirectory(whisper.cpp EXCLUDE_FROM_ALL)
endif()
