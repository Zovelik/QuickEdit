#pragma once

#include <media/AudioBuffer.h>

#include <core/Time.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>


namespace media
{
    // Properties of a media file, read by probe().
    struct ProbeResult
    {
        bool hasVideo = false;
        bool hasAudio = false;

        // A single picture (image file, or a video stream of one frame).
        bool isImage = false;

        core::Time duration = 0;

        uint32_t width = 0;
        uint32_t height = 0;
        core::Rational frameRate = { 30, 1 };

        uint32_t sampleRate = 0;
        uint32_t channels = 0;

        std::string videoCodec;
        std::string audioCodec;
    };


    // Reads the streams of a file. Throws std::runtime_error when the file
    // cannot be read.
    ProbeResult probe(const std::filesystem::path& path);


    // Decodes the first audio stream of a file to interleaved stereo float
    // samples at the given sample rate (mono is duplicated, more channels are
    // downmixed). Uses the frame threads of the decoder.
    //
    // progress, when set, is called with values in [0, 1]. The decoding stops
    // early, returning what was decoded, when cancel becomes true. Throws
    // std::runtime_error when the file has no readable audio.
    AudioBuffer decodeAudio(
        const std::filesystem::path& path,
        uint32_t sampleRate,
        const std::atomic<bool>& cancel,
        const std::function<void(float)>& progress = {});
}
