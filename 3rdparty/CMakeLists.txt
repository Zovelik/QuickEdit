add_subdirectory(SDL)
add_subdirectory(imgui)

# HTTP server of the MCP server (header only), MIT.
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE cpp-httplib)
find_package(Threads REQUIRED)
target_link_libraries(httplib INTERFACE Threads::Threads)

if (WIN32)
    target_link_libraries(httplib INTERFACE ws2_32)
endif()

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

    # For the processors of the users, not the one of the machine building
    # it (an AVX-512 build machine gives an illegal instruction elsewhere):
    # AVX2, FMA and F16C, every x86-64 processor since 2013-2015.
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    set(GGML_AVX ON CACHE BOOL "" FORCE)
    set(GGML_AVX2 ON CACHE BOOL "" FORCE)
    set(GGML_FMA ON CACHE BOOL "" FORCE)
    set(GGML_F16C ON CACHE BOOL "" FORCE)

    add_subdirectory(whisper.cpp EXCLUDE_FROM_ALL)
endif()
