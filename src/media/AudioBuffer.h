#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>


namespace media
{
    // Decoded audio of a media, interleaved stereo float samples at the
    // sample rate of the sequence. Immutable once published: shared between
    // the UI and the audio thread through shared_ptr<const AudioBuffer>.
    struct AudioBuffer
    {
        static constexpr uint32_t Channels = 2;

        uint32_t sampleRate = 0;
        std::vector<float> samples;

        int64_t frameCount() const { return (int64_t)(samples.size() / Channels); }
    };


    // Peaks of an audio buffer for drawing waveforms at any zoom level.
    // Level 0 holds the min and max of each block of BaseBlockSize frames
    // (both channels mixed), each next level halves the resolution.
    struct Waveform
    {
        static constexpr int64_t BaseBlockSize = 64;

        struct Peak
        {
            float min;
            float max;
        };

        uint32_t sampleRate = 0;
        std::vector<std::vector<Peak>> levels;

        // Frames covered by a peak of the given level.
        static int64_t blockSize(size_t level) { return BaseBlockSize << level; }

        static Waveform build(const AudioBuffer& buffer);
    };
}
