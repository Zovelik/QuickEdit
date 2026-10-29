add_executable(QuickEditCoreTests CoreTests.cpp)
target_link_libraries(QuickEditCoreTests PRIVATE QuickEditCore QuickEditMedia)

if (MSVC)
    target_compile_options(QuickEditCoreTests PRIVATE /utf-8)
endif()

add_test(NAME CoreTests COMMAND QuickEditCoreTests)

if (WIN32)
    # FFmpeg DLLs next to the executable
    add_custom_command(TARGET QuickEditCoreTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} $<TARGET_FILE_DIR:QuickEditCoreTests>
        COMMAND_EXPAND_LISTS
    )
endif()
