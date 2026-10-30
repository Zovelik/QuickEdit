#pragma once

#include <filesystem>


namespace core
{
    // Folder of the settings and caches of QuickEdit, where the system keeps
    // those of the applications: %APPDATA%\QuickEdit on Windows,
    // $XDG_CONFIG_HOME/quickedit (~/.config/quickedit) elsewhere. Created
    // when missing.
    std::filesystem::path configDirectory();
}
