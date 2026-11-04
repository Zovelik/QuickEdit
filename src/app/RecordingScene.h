#pragma once

#include <capture/ScreenRecorder.h>
#include <core/Project.h>

#include <imgui.h>

#include <array>
#include <string>
#include <vector>


// A scene of sources recorded together (screens, windows, cameras), each one
// placed in the frame of the sequence, as in OBS. Each source is recorded in
// a file of its own and placed on a track of its own, where it stays
// movable.
struct SceneSource
{
    capture::CaptureSource source;

    // Placed as a clip: the picture fitted in the frame, scaled and turned
    // around its center, moved from the center of the frame (fractions of
    // the width and height of the frame).
    core::FitMode fit = core::FitMode::Fit;
    float x = 0.f;
    float y = 0.f;
    float scale = 1.f;
    float rotation = 0.f;               // Degrees

    // Parts of the picture cut from each edge, in fractions of it (the rest
    // stays in place, as the crop of the clips).
    float cropLeft = 0.f;
    float cropTop = 0.f;
    float cropRight = 0.f;
    float cropBottom = 0.f;

    // False when the source of a saved scene is not there any more (window
    // closed, camera unplugged): it is not recorded.
    bool available = true;
};


struct RecordingScene
{
    std::string name;
    std::vector<SceneSource> sources;       // Bottom first
};


// A source in a rectangle of the frame (fractions of its size, from the top
// left, as the scenes were saved before), its picture fitted in it.
void placeInRectangle(SceneSource& source, float x, float y, float width, float height);

// The picture of a source fitted in the frame, at a scale of 1, in pixels
// of the frame (16:9 while the size of its pictures is not known).
ImVec2 sourceHalfSize(const SceneSource& source, float frameWidth, float frameHeight, int pictureWidth, int pictureHeight);

// The corners of the part of a source shown (its crop taken off), in pixels
// of the frame: top left, top right, bottom right, bottom left.
std::array<ImVec2, 4> sourceCorners(const SceneSource& source, float frameWidth, float frameHeight, int pictureWidth, int pictureHeight);

// The scenes saved in the settings folder (scenes.json). The sources are
// found again by kind and name (by device for the cameras) among the ones
// there now.
std::vector<RecordingScene> loadScenes(const std::vector<capture::CaptureSource>& available);
void saveScenes(const std::vector<RecordingScene>& scenes);
