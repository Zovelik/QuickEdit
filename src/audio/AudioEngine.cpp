#include <audio/AudioEngine.h>

#include <algorithm>


using namespace core;


namespace audio
{

namespace {

constexpr int BytesPerFrame = (int)(media::AudioBuffer::Channels * sizeof(float));

// Frames mixed at once, at most: about 20 ms at 48 kHz.
constexpr int64_t MaxChunkFrames = 1024;

} // namespace


AudioEngine::AudioEngine()
{
    _scratch.resize((size_t)(MaxChunkFrames * media::AudioBuffer::Channels));
    _graph.store(std::make_shared<MixGraph>());

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("Could not initialize the audio: %s", SDL_GetError());
        return;
    }

    const SDL_AudioSpec spec = { SDL_AUDIO_F32, (int)media::AudioBuffer::Channels, (int)_sampleRate };

    // The device starts paused, see play().
    _stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioEngine::callback, this);

    if (!_stream) {
        SDL_Log("Could not open the audio device: %s", SDL_GetError());
    }
}


AudioEngine::~AudioEngine()
{
    if (_stream) {
        SDL_DestroyAudioStream(_stream);
        _stream = nullptr;
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}


void AudioEngine::setGraph(std::shared_ptr<const MixGraph> graph)
{
    if (graph->sampleRate != _sampleRate) {
        setSampleRate(graph->sampleRate);
    }

    _graph.store(std::move(graph));
}


void AudioEngine::setSampleRate(uint32_t sampleRate)
{
    // The position is in frames of the old rate: restart from the same time.
    const bool wasPlaying = playing();
    const Time position = wasPlaying ? this->position() : 0;

    if (wasPlaying) {
        stop();
    }

    _sampleRate = sampleRate;

    if (_stream) {
        const SDL_AudioSpec spec = { SDL_AUDIO_F32, (int)media::AudioBuffer::Channels, (int)_sampleRate };
        SDL_SetAudioStreamFormat(_stream, &spec, nullptr);
    }

    if (wasPlaying) {
        play(position);
    }
}


void AudioEngine::play(Time from)
{
    stop();

    _startTime = from;
    _startNS = SDL_GetTicksNS();
    _mixFrame = mulDivFloor(from, _sampleRate, TicksPerSecond);
    _playing = true;

    if (_stream) {
        SDL_ResumeAudioStreamDevice(_stream);
    }
}


void AudioEngine::stop()
{
    _playing = false;

    if (_stream) {
        SDL_PauseAudioStreamDevice(_stream);
        SDL_ClearAudioStream(_stream);
    }
}


Time AudioEngine::position() const
{
    if (!_stream) {
        // Nanoseconds to ticks: 705600000 / 1e9 = 7056 / 10000.
        return _startTime + mulDivFloor((int64_t)(SDL_GetTicksNS() - _startNS), 7056, 10000);
    }

    // Mixed frames minus those still waiting in the stream. The device
    // buffer adds a few milliseconds of latency on top, not accounted for.
    const int queued = std::max(0, SDL_GetAudioStreamQueued(_stream)) / BytesPerFrame;
    const int64_t frame = std::max<int64_t>(0, _mixFrame.load() - queued);
    const Time position = mulDivFloor(frame, TicksPerSecond, _sampleRate);

    return std::max(position, _startTime);
}


void SDLCALL AudioEngine::callback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount)
{
    AudioEngine* engine = static_cast<AudioEngine*>(userdata);

    if (!engine->_playing) {
        return;
    }

    const std::shared_ptr<const MixGraph> graph = engine->_graph.load();
    int64_t frames = additionalAmount / BytesPerFrame;

    while (frames > 0) {
        const int64_t chunk = std::min(frames, MaxChunkFrames);
        const int64_t first = engine->_mixFrame.load();

        mix(*graph, first, chunk, engine->_scratch.data());
        SDL_PutAudioStreamData(stream, engine->_scratch.data(), (int)(chunk * BytesPerFrame));

        engine->_mixFrame = first + chunk;
        frames -= chunk;
    }
}

}
