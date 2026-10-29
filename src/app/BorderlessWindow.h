#pragma once

#include <SDL3/SDL.h>


// On Windows, SDL maximizes borderless windows to the whole screen, covering
// the taskbar (see WM_GETMINMAXINFO in SDL_windowsevents.c). This makes the
// window maximize to the monitor's work area instead, whatever triggers the
// maximization (button, caption double-click, snapping, Win+Up...).
//
// No-op on other platforms.
void fixBorderlessMaximize(SDL_Window* window);
