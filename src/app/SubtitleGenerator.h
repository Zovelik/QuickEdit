#pragma once

#include <core/Subtitles.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


// Speech recognition models (whisper.cpp), from the smallest to the most
// accurate. The user picks one for the power of the computer: the large
// ones want a GPU.
struct WhisperModel
{
    const char* id;
    const char* name;
    const char* file;               // In modelFolder(), downloaded from ModelUrl
    int sizeMegabytes;
    const char* description;
};

const std::vector<WhisperModel>& whisperModels();

// Documents/QuickEdit/Models
std::filesystem::path modelFolder();

bool modelDownloaded(const WhisperModel& model);

// Whether this build has speech recognition (whisper.cpp found at build).
bool speechRecognitionAvailable();


// A download on a thread of its own, through FFmpeg (https), into a file
// renamed when complete.
class ModelDownload
{
public:
    ~ModelDownload();

    void start(const WhisperModel& model);
    void cancel() { _cancel = true; }

    bool running() const { return _running.load(); }
    float progress() const { return _progress.load(); }
    const std::string& modelId() const { return _modelId; }
    std::string error() const;

private:
    void run(std::string url, std::filesystem::path path);

    std::thread _thread;
    std::atomic<bool> _running = false;
    std::atomic<bool> _cancel = false;
    std::atomic<float> _progress = 0.f;
    std::string _modelId;

    mutable std::mutex _mutex;
    std::string _error;
};


// Subtitles from the sound of a sequence, on a thread of their own: the
// samples (16 kHz mono) through a model, its segments as cues.
class SubtitleGenerator
{
public:
    struct Settings
    {
        std::filesystem::path model;
        std::string language = "auto";  // Or "fr", "en"...
        bool useGpu = true;
        int maxCharacters = 60;         // Per line of subtitles, about

        // Words to expect (names, jargon), given to the model as the text
        // before: "QuickEdit, Vulkan, MCP".
        std::string prompt;
    };

    ~SubtitleGenerator();

    void start(std::vector<float> samples, const Settings& settings);
    void cancel() { _cancel = true; }

    bool running() const { return _running.load(); }
    bool done() const { return _done.load(); }
    float progress() const { return _progress.load(); }

    // The results taken: done no more.
    void acknowledge() { _done = false; }

    // Once done: the cues, or why there are none.
    std::vector<core::SubtitleCue> cues() const;
    std::string error() const;

private:
    void run(std::vector<float> samples, Settings settings);

    std::thread _thread;
    std::atomic<bool> _running = false;
    std::atomic<bool> _done = false;
    std::atomic<bool> _cancel = false;
    std::atomic<float> _progress = 0.f;

    mutable std::mutex _mutex;
    std::vector<core::SubtitleCue> _cues;
    std::string _error;
};
