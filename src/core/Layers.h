#pragma once

#include <core/Project.h>

#include <cstdint>
#include <functional>
#include <vector>


namespace core
{
    // A picture to draw at a time of the sequence: a clip of a video track,
    // with what a transition adds to its own transform.
    struct LayerRequest
    {
        const Clip* clip = nullptr;
        size_t track = 0;               // Video track index, bottom first

        // Consecutive clips of a track take turns between slots 0 and 1, each
        // with its own decoder: the next clip is decoded ahead while the
        // current one plays, and both clips of a transition have theirs.
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

        // A shader transition: 1 for the outgoing picture, 2 for the
        // incoming one (drawn together by the shader), at progress.
        int transitionRole = 0;
        float transitionProgress = 0.f;
        const Transition* transition = nullptr;

        // The decoder of the layer: one per track, slot and level of
        // nesting, the same from frame to frame.
        uint64_t key = 0;

        // A nested sequence: it, and its layers at the time shown.
        const Sequence* nested = nullptr;
        std::vector<LayerRequest> children;
    };


    // The layers of the sequence at time t, bottom first.
    //
    // A transition happens around the cut between two clips of a track, over
    // its duration centered on the cut. No media beyond the clips is needed:
    // before the cut, the incoming clip shows its first frame; after it, the
    // outgoing clip shows its last frame. Without an outgoing clip, the
    // incoming one appears over what is below.
    std::vector<LayerRequest> layersAt(const Sequence& sequence, Time t,
        const std::function<const Sequence*(Id)>& sequenceOf = {}, uint64_t parentKey = 0, int depth = 0);


    // The clips starting after t, within the horizon (a nested sequence's
    // first layers with them, and the cuts inside the nested sequences
    // playing at t): the first frame of each, to decode ahead of the cut.
    // Their keys are the ones layersAt() gives them then; nested layers
    // come flattened.
    std::vector<LayerRequest> upcomingLayers(const Sequence& sequence, Time t, Time horizon,
        const std::function<const Sequence*(Id)>& sequenceOf = {}, uint64_t parentKey = 0, int depth = 0);

    const char* transitionName(TransitionType type);
}
