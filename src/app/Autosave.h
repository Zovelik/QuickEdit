#pragma once

#include <core/Project.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>


// Emergency copies of the project being edited, in the settings folder
// (autosave/), so that a crash never loses the work: written now and then
// while the project has changes not saved, removed when the application
// closes normally. A last resort beside the normal save, which they do not
// replace.
namespace autosave
{
    // A copy left by a session that ended without closing: to recover.
    struct Leftover
    {
        std::filesystem::path file;         // The copy
        std::filesystem::path original;     // The project file, empty if never saved
        std::string title;
        int64_t time = 0;                   // When it was written, seconds since 1970
    };

    // The copy of this session, replaced each time. Never throws: a copy
    // that cannot be written is logged.
    void write(const core::Project& project, const std::filesystem::path& projectPath);

    // Removes the copy of this session (saved project, normal exit).
    void remove();

    // The copies of the sessions no longer running, the most recent first.
    std::vector<Leftover> leftovers();

    // The project of a copy (throws when it cannot be read).
    core::Project load(const Leftover& leftover);

    void discard(const Leftover& leftover);
}
