#pragma once

#include <media/AudioBuffer.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>


namespace audio
{
    struct InputDevice
    {
        SDL_AudioDeviceID id = 0;
        std::string name;
    };


    // Recording from an audio input (microphone), through SDL.
    //
    // While open, the level of the input is measured for the meter; between
    // start() and stop(), the samples are also written to a WAV file (mono,
    // 24 bits). SDL delivers them on its own thread (callback()).
    class Recorder
    {
    public:
        static constexpr uint32_t SampleRate = 48000;

        Recorder();
        ~Recorder();

        Recorder(const Recorder&) = delete;
        Recorder& operator=(const Recorder&) = delete;

        // The inputs of the system.
        static std::vector<InputDevice> devices();

        // Opens an input, 0 for the default one. False with error set when it
        // cannot be opened.
        bool open(SDL_AudioDeviceID device, std::string& error);
        void close();
        bool isOpen() const { return _stream != nullptr; }
        SDL_AudioDeviceID device() const { return _device; }

        // Highest absolute sample value since the previous call.
        float takePeak() { return _peak.exchange(0.f); }

        // Writes the input to a new file from now on.
        bool start(const std::filesystem::path& path, std::string& error);

        // Stops writing, and returns the number of frames written.
        int64_t stop();

        bool recording() const { return _recording.load(); }
        int64_t recordedFrames() const { return _frames.load(); }

        // Appends the peaks of the samples written since the previous call
        // (one per Waveform::BaseBlockSize frames), for the waveform drawn
        // while recording.
        void takePeaks(std::vector<media::Waveform::Peak>& peaks);

    private:
        static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);

        // Sizes of the RIFF header, from the frames written.
        void writeHeader();

        SDL_AudioStream* _stream = nullptr;
        SDL_AudioDeviceID _device = 0;

        std::atomic<float> _peak = 0.f;

        std::mutex _fileMutex;
        std::FILE* _file = nullptr;
        std::atomic<bool> _recording = false;
        std::atomic<int64_t> _frames = 0;
        int64_t _framesInHeader = 0;

        // Peaks not taken yet, and the block being measured (audio thread).
        std::mutex _peaksMutex;
        std::vector<media::Waveform::Peak> _peaks;
        media::Waveform::Peak _block = { 0.f, 0.f };
        int64_t _blockFrames = 0;

        // Buffers of the audio thread.
        std::vector<float> _samples;
        std::vector<uint8_t> _bytes;
    };
}
