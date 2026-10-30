#include <core/Paths.h>

#include <cstdlib>
#include <system_error>


namespace core
{

std::filesystem::path configDirectory()
{
    std::filesystem::path folder;

#ifdef _WIN32
    if (const wchar_t* appData = _wgetenv(L"APPDATA")) {
        folder = std::filesystem::path(appData) / "QuickEdit";
    }
#else
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config) {
        folder = std::filesystem::path(config) / "quickedit";
    }
    else if (const char* home = std::getenv("HOME")) {
        folder = std::filesystem::path(home) / ".config" / "quickedit";
    }
#endif

    if (folder.empty()) {
        folder = std::filesystem::temp_directory_path() / "QuickEdit";
    }

    std::error_code error;
    std::filesystem::create_directories(folder, error);

    return folder;
}

}
