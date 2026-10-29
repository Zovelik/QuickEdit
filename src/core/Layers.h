#pragma once

#include <core/Project.h>

#include <vector>


namespace core
{
    // A picture to draw at a time of the sequence: a clip of a video track,
    // with what a transition adds to its own transform.
    struct LayerRequest
    {
        const Clip* clip = nullptr;
        size_t track = 0;               // Video track index, bottom first

        // 0 for the clip at the time on its track, 1 for the other clip of a
        // transition. Each slot of a track has its own decoder.
        int slot = 0;

        Time localTime = 0;             // For the animations, in [0, duration]
        Time sourceTime = 0;            // In the media (media clips)

        // The picture does not move: a still of a transition, or a freeze
        // frame. No need to decode ahead.
        bool still = false;

        // Transition
        float opacity = 1.f;
        float offsetX = 0.f;            // Pixels of the sequence
        float offsetY = 0.f;
        float scale = 1.f;
        float wipe = 1.f;               // Part of the width shown, from the left
    };


    // The layers of the sequence at time t, bottom first.
    //
    // A transition happens around the cut between two clips of a track, over
    // its duration centered on the cut. No media beyond the clips is needed:
    // before the cut, the incoming clip shows its first frame; after it, the
    // outgoing clip shows its last frame. Without an outgoing clip, the
    // incoming one appears over what is below.
    std::vector<LayerRequest> layersAt(const Sequence& sequence, Time t);


    const char* transitionName(TransitionType type);
}
