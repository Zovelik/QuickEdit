#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <vector>


// Files dragged from the file manager over the window.
struct FileDrag
{
    bool active = false;

    // Dropped: the application takes the files, then ends the drag.
    bool dropped = false;

    // Known from the start of the drag on Windows, only once dropped on the
    // other platforms.
    std::vector<std::filesystem::path> files;

    // Mouse, in window coordinates.
    float x = 0.f;
    float y = 0.f;
};


// On Windows, replaces the drop target of SDL by one that reads the paths of
// the files as soon as they enter the window (SDL gives them on the drop
// only), and keeps drag up to date. Called on the main thread, while SDL
// pumps the messages. False on the other platforms: SDL events then.
bool registerFileDropTarget(SDL_Window* window, FileDrag& drag);
