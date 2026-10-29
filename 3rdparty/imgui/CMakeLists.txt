find_package(Vulkan REQUIRED)

add_library(imgui 
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/misc/cpp/imgui_stdlib.cpp
    #imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui PUBLIC imgui imgui/misc/cpp)
target_link_libraries(imgui PUBLIC SDL3::SDL3)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan)