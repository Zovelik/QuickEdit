#pragma once

#include <audio/Mixer.h>

#include <core/Time.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <memory>
#include <vector>


namespace audio
{
    // Playback of the mix graph on the default audio device, through SDL.
    //
    // SDL pulls the samples from its own thread (callback()), which mixes
    // the current graph. The graph is swapped atomically by the main thread
    // with setGraph(). During playback, the position of the samples actually
    // queued for the device is the master clock: the video follows it.
    //
    // Without an audio device, playback still runs on the system clock.
    class AudioEngine
    {
    public:
        AudioEngine();
        ~AudioEngine();

        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        void setGraph(std::shared_ptr<const MixGraph> graph);

        void play(core::Time from);
        void stop();
        bool playing() const { return _playing.load(); }

        // Time being heard while playing.
        core::Time position() const;

        bool hasDevice() const { return _stream != nullptr; }

    private:
        static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);

        // Sets the sample rate of the samples given to SDL, which converts
        // them to the rate of the device.
        void setSampleRate(uint32_t sampleRate);

        SDL_AudioStream* _stream = nullptr;
        uint32_t _sampleRate = 48000;

        std::atomic<std::shared_ptr<const MixGraph>> _graph;
        std::atomic<bool> _playing = false;

        // Next frame to mix (audio thread) and start of the playback.
        std::atomic<int64_t> _mixFrame = 0;
        core::Time _startTime = 0;
        Uint64 _startNS = 0;

        // Mixing buffer of the audio thread.
        std::vector<float> _scratch;
    };
}
