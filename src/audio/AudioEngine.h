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

        // While stopped: plays a short piece of the sound at time t (the
        // playhead dragged, a frame step). A piece still playing goes on
        // from t. Nothing when t is the time of the last piece.
        void scrub(core::Time t);

        bool hasDevice() const { return _stream != nullptr; }

    private:
        static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);

        // The rest of the scrub piece, while stopped (audio thread).
        void scrubCallback(SDL_AudioStream* stream, int additionalAmount);

        // Sets the sample rate of the samples given to SDL, which converts
        // them to the rate of the device.
        void setSampleRate(uint32_t sampleRate);

        SDL_AudioStream* _stream = nullptr;
        std::atomic<uint32_t> _sampleRate = 48000;     // Read by the audio thread

        std::atomic<std::shared_ptr<const MixGraph>> _graph;
        std::atomic<bool> _playing = false;

        // Next frame to mix (audio thread) and start of the playback.
        std::atomic<int64_t> _mixFrame = 0;
        core::Time _startTime = 0;
        Uint64 _startNS = 0;

        // Scrubbing: next frame to mix and frames left of the piece, of
        // scrubLength (audio thread), and the time of the last piece.
        std::atomic<int64_t> _scrubFrame = 0;
        std::atomic<int64_t> _scrubLeft = 0;
        std::atomic<int64_t> _scrubPlayed = 0;      // Frames since the sound started, for its fade in
        std::atomic<int64_t> _scrubLength = 0;
        core::Time _lastScrub = -1;

        // Mixing buffer of the audio thread.
        std::vector<float> _scratch;
    };
}
