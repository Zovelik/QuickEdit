#include <app/Autosave.h>

#include <core/Json.h>
#include <core/Paths.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif


using namespace core;


namespace autosave
{

namespace {

std::filesystem::path folder()
{
    return configDirectory() / "autosave";
}


unsigned long processId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}


// Whether the process of a session still runs (its copy is not left over).
bool running(unsigned long pid)
{
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);

    if (!process) {
        return false;
    }

    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0;
#endif
}


// The copy of a session and its description, beside it.
std::filesystem::path projectFile(unsigned long pid)
{
    return folder() / ("session-" + std::to_string(pid) + ".qep");
}


std::filesystem::path infoFile(const std::filesystem::path& project)
{
    std::filesystem::path info = project;
    info.replace_extension(".json");
    return info;
}


std::string readText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}


// Written next to the destination, then put in its place: never half a file.
bool writeText(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::path temp = path;
    temp += ".tmp";

    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(text.data(), (std::streamsize)text.size());

        if (!file) {
            return false;
        }
    }

    std::error_code error;
    std::filesystem::rename(temp, path, error);
    return !error;
}

} // namespace


void write(const Project& project, const std::filesystem::path& projectPath)
{
    try {
        std::error_code error;
        std::filesystem::create_directories(folder(), error);

        // Absolute paths: the copy is not next to the project.
        const std::filesystem::path file = projectFile(processId());
        const std::u8string original = projectPath.u8string();
        const std::u8string title = projectPath.stem().u8string();

        Json info = Json::object();
        info["original"] = std::string(original.begin(), original.end());
        info["title"] = std::string(title.begin(), title.end());
        info["time"] = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        if (!writeText(file, project.toJson({}).dump()) || !writeText(infoFile(file), info.dump())) {
            SDL_Log("Autosave: could not write %s", file.string().c_str());
        }
    }
    catch (const std::exception& e) {
        SDL_Log("Autosave: %s", e.what());
    }
}


void remove()
{
    std::error_code error;
    const std::filesystem::path file = projectFile(processId());
    std::filesystem::remove(file, error);
    std::filesystem::remove(infoFile(file), error);
}


std::vector<Leftover> leftovers()
{
    std::vector<Leftover> list;
    std::error_code error;

    for (const auto& entry : std::filesystem::directory_iterator(folder(), error)) {
        const std::filesystem::path& path = entry.path();
        const std::string name = path.stem().string();

        if (path.extension() != ".qep" || name.rfind("session-", 0) != 0) {
            continue;
        }

        unsigned long pid = 0;

        try {
            pid = std::stoul(name.substr(8));
        }
        catch (const std::exception&) {
            continue;
        }

        if (pid == processId() || running(pid)) {
            continue;
        }

        Leftover leftover;
        leftover.file = path;

        try {
            const Json info = Json::parse(readText(infoFile(path)));
            const std::string original = info["original"].toString();
            leftover.original = std::filesystem::path(std::u8string(original.begin(), original.end()));
            leftover.title = info["title"].toString();
            leftover.time = info["time"].toInt(0);
        }
        catch (const std::exception&) {
            // Without its description: still recoverable, untitled.
        }

        list.push_back(std::move(leftover));
    }

    std::sort(list.begin(), list.end(), [](const Leftover& a, const Leftover& b) { return a.time > b.time; });
    return list;
}


Project load(const Leftover& leftover)
{
    Project project;
    project.fromJson(Json::parse(readText(leftover.file)), {});
    return project;
}


void discard(const Leftover& leftover)
{
    std::error_code error;
    std::filesystem::remove(leftover.file, error);
    std::filesystem::remove(infoFile(leftover.file), error);
}

}
