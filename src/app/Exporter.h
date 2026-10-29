#pragma once

#include <audio/Mixer.h>
#include <media/AudioBuffer.h>

#include <core/Project.h>

#include <vkutils/Device.h>
#include <vkutils/PipelineCache.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVBufferRef;


struct ExportSettings
{
    enum class Codec
    {
        H264,
        H265,
        AV1
    };

    std::filesystem::path path;         // .mp4, .mov or .mkv
    Codec codec = Codec::H264;
    float bitrateMbps = 16.f;
    bool audio = true;

    // Height of the video, the width following the shape of the sequence;
    // 0 for the size of the sequence. The placements stay relative to the
    // frame.
    uint32_t height = 0;

    // Frames per second; 0 for the rate of the sequence.
    core::Rational frameRate = { 0, 1 };
};


// What the exporter shares with the application.
struct ExportContext
{
    const GPU::Device* device = nullptr;
    const GPU::PipelineCache* pipelineCache = nullptr;

    // Queue of the graphics family for the composition, and the lock of its
    // submissions (shared with the application and FFmpeg).
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex* queueMutex = nullptr;

    // FFmpeg Vulkan device (decoding, encoding), nullptr for the CPU.
    AVBufferRef* hwDevice = nullptr;
    bool vulkanEncode = false;

    // TTF data of the default font of the texts.
    std::vector<uint8_t> defaultFont;
};


// Renders a sequence to a video file on its own thread: each frame is
// composited on the GPU as in the viewer, converted to NV12 on the GPU and
// encoded by Vulkan Video when available (the picture never leaves the
// GPU), otherwise read back and given to the first hardware encoder that
// works (NVENC, AMF, Quick Sync, Media Foundation). The audio is mixed as
// for playback and encoded in AAC.
class Exporter
{
public:
    // project: a copy, the exporter works on it while the edits go on. audio:
    // the decoded audio of the media, at the sample rate of the sequence.
    // effects: instances of the effects of the audio tracks (by track id),
    // for the export alone.
    Exporter(ExportContext context, core::Project project,
        std::map<core::Id, std::shared_ptr<const media::AudioBuffer>> audio,
        std::map<core::Id, std::vector<std::shared_ptr<audio::AudioEffect>>> effects,
        ExportSettings settings);

    // Cancels the export if still running, and waits for its thread.
    ~Exporter();

    void cancel() { _cancel = true; }

    // Progress in [0, 1], and the state once done.
    float progress() const { return _progress.load(); }
    bool done() const { return _done.load(); }
    bool succeeded() const { return _succeeded.load(); }
    bool cancelled() const { return _cancel.load(); }

    // Error when it failed. Encoder in use, once chosen.
    std::string error() const;
    std::string encoder() const;

    const ExportSettings& settings() const { return _settings; }

private:
    void run();
    void exportVideo();

    ExportContext _context;
    core::Project _project;
    std::map<core::Id, std::shared_ptr<const media::AudioBuffer>> _audio;
    std::map<core::Id, std::vector<std::shared_ptr<audio::AudioEffect>>> _effects;
    ExportSettings _settings;

    std::atomic<float> _progress = 0.f;
    std::atomic<bool> _cancel = false;
    std::atomic<bool> _done = false;
    std::atomic<bool> _succeeded = false;

    mutable std::mutex _mutex;
    std::string _error;
    std::string _encoder;

    std::thread _thread;
};
