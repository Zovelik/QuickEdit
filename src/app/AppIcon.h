#pragma once

#include <imgui.h>


// Draws the application icon, a play triangle above two timeline clips on a
// dark tile, in the square of the given size (in pixels) whose top left
// corner is at pos.
//
// The icon is drawn from vector shapes so it stays sharp whatever the display
// scale. The icon of the executable (src/resources/icon.ico), which the OS
// shows in the launcher and the taskbar, is generated from the same design by
// tools/generate_icon.py: keep both in sync.
void drawAppIcon(ImDrawList* drawList, const ImVec2& pos, float size);
