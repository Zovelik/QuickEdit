#pragma once

#include <capture/ScreenRecorder.h>

#include <string>
#include <vector>


// A scene of sources recorded together (screens, windows, cameras), each one
// placed in the frame of the sequence, as in OBS. Each source is recorded in
// a file of its own and placed on a track of its own, where it stays
// movable.
struct SceneSource
{
    capture::CaptureSource source;

    // Rectangle in the frame, in fractions of its size, from the top left.
    float x = 0.f;
    float y = 0.f;
    float width = 1.f;
    float height = 1.f;

    // False when the source of a saved scene is not there any more (window
    // closed, camera unplugged): it is not recorded.
    bool available = true;
};


struct RecordingScene
{
    std::string name;
    std::vector<SceneSource> sources;       // Bottom first
};


// The scenes saved in the settings folder (scenes.json). The sources are
// found again by kind and name (by device for the cameras) among the ones
// there now.
std::vector<RecordingScene> loadScenes(const std::vector<capture::CaptureSource>& available);
void saveScenes(const std::vector<RecordingScene>& scenes);
