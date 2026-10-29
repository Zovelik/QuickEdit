#pragma once

#include <media/AudioBuffer.h>

#include <core/Project.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>


namespace audio
{
    // An audible clip, in frames (one frame = one sample per channel) of the
    // sample rate of the sequence.
    // An effect processing a track (a plugin), in place, on interleaved
    // stereo samples. Called by one thread at a time, block after block.
    class AudioEffect
    {
    public:
        virtual ~AudioEffect() = default;
        virtual void process(float* samples, int frames, int64_t position) = 0;
    };


    struct MixClip
    {
        int64_t start = 0;          // On the timeline
        int64_t length = 0;
        int64_t sourceStart = 0;    // In the buffer
        float gain = 1.f;           // Linear
        int64_t fadeIn = 0;         // Frames
        int64_t fadeOut = 0;
        std::shared_ptr<const media::AudioBuffer> buffer;

        // Effects of the clip, before those of its track.
        std::vector<std::shared_ptr<AudioEffect>> effects;

        // Source frames per frame of the timeline.
        double speed = 1.0;
    };


    struct MixTrack
    {
        std::vector<MixClip> clips;

        // The tracks of the sequences nested on the video track of a sound
        // track: each through its own effects, mixed with the clips before
        // the effects of the track.
        std::vector<MixTrack> subTracks;

        std::vector<std::shared_ptr<AudioEffect>> effects;
    };


    // What the audio thread plays: an immutable flattening of the audio
    // tracks of a sequence, rebuilt by the main thread after each edit and
    // swapped atomically.
    struct MixGraph
    {
        uint32_t sampleRate = 48000;
        std::vector<MixTrack> tracks;
    };


    // Builds the graph of the audible tracks: muted tracks are skipped, and
    // when a track is solo only the solo tracks are heard. Clips whose audio
    // is not decoded yet are skipped. effectsOf, when set, gives the effects
    // of a track or a clip. The sound of a nested sequence goes through the
    // effects of its clips and tracks, then those of the parent track. A
    // sequence nested more than once has its effects on its first use only:
    // a plugin processes one stream.
    std::shared_ptr<const MixGraph> buildMixGraph(
        const core::Project& project,
        const std::function<std::shared_ptr<const media::AudioBuffer>(core::Id mediaId)>& audioOf,
        const std::function<std::vector<std::shared_ptr<AudioEffect>>(core::Id owner, const std::vector<core::EffectSlot>& slots)>& effectsOf = {});


    // Mixes the frames [first, first + count) of the graph into output,
    // interleaved stereo, overwriting it: each track, through its effects,
    // then their sum. Clip edges get a few milliseconds of fade so that cuts
    // never click. Real-time safe: no lock, and no allocation once the
    // scratch buffer of the thread is as large as count.
    void mix(const MixGraph& graph, int64_t first, int64_t count, float* output);
}
