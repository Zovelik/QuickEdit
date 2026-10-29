#include <iostream>
#include <exception>

#ifdef WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <filesystem>

#include <app/GUI.h>
#include <config.h>

extern "C" {
#include <libavutil/log.h>
}

#include <cstdarg>
#include <cstdlib>
#include <string>
#include <cstdio>
#include <mutex>


namespace {

#ifndef NDEBUG
// Debug builds have no console either: the SDL, Vulkan validation and FFmpeg
// messages go to quickedit.log, next to the executable.
std::FILE* logFile = nullptr;
std::mutex logMutex;

void SDLCALL logToFile(void* userdata, int category, SDL_LogPriority priority, const char* message)
{
    std::scoped_lock lock(logMutex);

    if (logFile) {
        std::fprintf(logFile, "%s\n", message);
        std::fflush(logFile);
    }
}

void ffmpegLog(void* context, int level, const char* format, va_list arguments)
{
    if (level > AV_LOG_WARNING) {
        return;
    }

    char line[1024];
    int prefix = 1;
    av_log_format_line(context, level, format, arguments, line, sizeof(line), &prefix);
    SDL_Log("[FFmpeg] %s", line);
}

void startLog()
{
    logFile = std::fopen("quickedit.log", "w");
    SDL_SetLogOutputFunction(logToFile, nullptr);
    av_log_set_callback(ffmpegLog);
}
#else
void startLog() {}
#endif

} // namespace

#ifdef WIN32
int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
#else
int main(int argc, char* argv[])
#endif
{
    // A project given on the command line (e.g. opened from the Explorer),
    // and optionally the time to show and a clip to select (for tests):
    // QuickEdit project.qep [--time seconds] [--select clipId]
    //                       [--edit] [--effect-editor slotId]
    //                       [--export video.mp4 [--codec h264|h265|av1] [--height h] [--fps n]]
    //                       [--drop file x y] [--record seconds]
    //                       [--quit-after seconds] [--record-screen seconds [--source index]]
    std::filesystem::path projectPath;
    std::filesystem::path exportPath;
    std::string codec = "h264";
    double startTime = 0.0;
    uint64_t selectedClip = 0;
    bool editText = false;
    uint64_t effectEditor = 0;
    std::filesystem::path dropPath;
    double recordSeconds = 0.0;
    double quitAfter = 0.0;
    uint32_t exportHeight = 0;
    int exportFps = 0;
    double recordScreen = 0.0;
    bool play = false;
    int screenSource = 0;
    float dropX = 0.f, dropY = 0.f;

#ifdef WIN32
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (std::wstring(argv[i]) == L"--time" && i + 1 < argc) {
                startTime = std::wcstod(argv[++i], nullptr);
            }
            else if (std::wstring(argv[i]) == L"--select" && i + 1 < argc) {
                selectedClip = std::wcstoull(argv[++i], nullptr, 10);
            }
            else if (std::wstring(argv[i]) == L"--effect-editor" && i + 1 < argc) {
                effectEditor = std::wcstoull(argv[++i], nullptr, 10);
            }
            else if (std::wstring(argv[i]) == L"--drop" && i + 3 < argc) {
                dropPath = argv[++i];
                dropX = std::wcstof(argv[++i], nullptr);
                dropY = std::wcstof(argv[++i], nullptr);
            }
            else if (std::wstring(argv[i]) == L"--source" && i + 1 < argc) {
                screenSource = (int)std::wcstol(argv[++i], nullptr, 10);
            }
            else if (std::wstring(argv[i]) == L"--record-screen" && i + 1 < argc) {
                recordScreen = std::wcstod(argv[++i], nullptr);
            }
            else if (std::wstring(argv[i]) == L"--height" && i + 1 < argc) {
                exportHeight = (uint32_t)std::wcstoul(argv[++i], nullptr, 10);
            }
            else if (std::wstring(argv[i]) == L"--fps" && i + 1 < argc) {
                exportFps = (int)std::wcstol(argv[++i], nullptr, 10);
            }
            else if (std::wstring(argv[i]) == L"--quit-after" && i + 1 < argc) {
                quitAfter = std::wcstod(argv[++i], nullptr);
            }
            else if (std::wstring(argv[i]) == L"--record" && i + 1 < argc) {
                recordSeconds = std::wcstod(argv[++i], nullptr);
            }
            else if (std::wstring(argv[i]) == L"--play") {
                play = true;
            }
            else if (std::wstring(argv[i]) == L"--edit") {
                editText = true;
            }
            else if (std::wstring(argv[i]) == L"--export" && i + 1 < argc) {
                exportPath = argv[++i];
            }
            else if (std::wstring(argv[i]) == L"--codec" && i + 1 < argc) {
                // ASCII codec names.
                codec.clear();

                for (const wchar_t* c = argv[++i]; *c; c++) {
                    codec += (char)(*c & 0x7F);
                }
            }
            else {
                projectPath = argv[i];
            }
        }

        LocalFree(argv);
    }
#else
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--time" && i + 1 < argc) {
            startTime = std::strtod(argv[++i], nullptr);
        }
        else if (std::string(argv[i]) == "--select" && i + 1 < argc) {
            selectedClip = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (std::string(argv[i]) == "--effect-editor" && i + 1 < argc) {
            effectEditor = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (std::string(argv[i]) == "--drop" && i + 3 < argc) {
            dropPath = std::filesystem::u8path(argv[++i]);
            dropX = std::strtof(argv[++i], nullptr);
            dropY = std::strtof(argv[++i], nullptr);
        }
        else if (std::string(argv[i]) == "--source" && i + 1 < argc) {
            screenSource = (int)std::strtol(argv[++i], nullptr, 10);
        }
        else if (std::string(argv[i]) == "--record-screen" && i + 1 < argc) {
            recordScreen = std::strtod(argv[++i], nullptr);
        }
        else if (std::string(argv[i]) == "--height" && i + 1 < argc) {
            exportHeight = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
        }
        else if (std::string(argv[i]) == "--fps" && i + 1 < argc) {
            exportFps = (int)std::strtol(argv[++i], nullptr, 10);
        }
        else if (std::string(argv[i]) == "--quit-after" && i + 1 < argc) {
            quitAfter = std::strtod(argv[++i], nullptr);
        }
        else if (std::string(argv[i]) == "--record" && i + 1 < argc) {
            recordSeconds = std::strtod(argv[++i], nullptr);
        }
        else if (std::string(argv[i]) == "--play") {
            play = true;
        }
        else if (std::string(argv[i]) == "--edit") {
            editText = true;
        }
        else if (std::string(argv[i]) == "--export" && i + 1 < argc) {
            exportPath = std::filesystem::u8path(argv[++i]);
        }
        else if (std::string(argv[i]) == "--codec" && i + 1 < argc) {
            codec = argv[++i];
        }
        else {
            projectPath = std::filesystem::u8path(argv[i]);
        }
    }
#endif

    startLog();

    try {
        GUI gui(projectPath, startTime, selectedClip);

        // The selected text in the viewer editor.
        if (editText) {
            gui.editSelectedText();
        }

        // The editor of an effect (slot id).
        if (effectEditor) {
            gui.openEffectEditor(effectEditor);
        }

        if (play) {
            gui.testPlay();
        }

        if (recordScreen > 0.0) {
            gui.testRecordScreen(recordScreen, screenSource);
        }

        if (quitAfter > 0.0) {
            gui.testQuitAfter(quitAfter);
        }

        if (recordSeconds > 0.0) {
            gui.testRecord(recordSeconds);
        }

        if (!dropPath.empty()) {
            gui.testFileDrop(dropPath, dropX, dropY);
        }

        if (!exportPath.empty()) {
            gui.exportAndQuit(exportPath,
                codec == "av1" ? ExportSettings::Codec::AV1 :
                codec == "h265" ? ExportSettings::Codec::H265 : ExportSettings::Codec::H264, exportHeight, exportFps);
        }

        gui.run();
    } catch (const std::exception& e) {
        // Also shown in a message box: there is no console to read it from
        // in a WIN32 application. SDL allows this call even after SDL_Quit().
        std::cerr << "Exception: " << e.what() << std::endl;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME_STR, e.what(), nullptr);
        return 1;
    }

    return 0;
}