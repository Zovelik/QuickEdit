#include <audio/Recorder.h>

#include <algorithm>
#include <cmath>
#include <cstring>


namespace audio
{

namespace {

constexpr int BytesPerSample = 3;
constexpr int HeaderSize = 44;


void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

} // namespace


Recorder::Recorder()
{
    SDL_InitSubSystem(SDL_INIT_AUDIO);
}


Recorder::~Recorder()
{
    close();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}


std::vector<InputDevice> Recorder::devices()
{
    std::vector<InputDevice> devices;

    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);

    for (int i = 0; ids && i < count; i++) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        devices.push_back({ ids[i], name ? name : "Input " + std::to_string(i + 1) });
        SDL_Log("Audio input: %s", devices.back().name.c_str());
    }

    SDL_free(ids);

    return devices;
}


bool Recorder::open(SDL_AudioDeviceID device, std::string& error)
{
    close();

    // Mono: SDL mixes the channels of the input down.
    const SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, (int)SampleRate };

    _stream = SDL_OpenAudioDeviceStream(device ? device : SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, Recorder::callback, this);

    if (!_stream) {
        error = SDL_GetError();
        return false;
    }

    _device = device;
    SDL_ResumeAudioStreamDevice(_stream);

    SDL_AudioSpec deviceSpec = {};
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(_stream), &deviceSpec, nullptr);
    const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(_stream));
    SDL_Log("Recording input: %s (%d Hz, %d channels)", name ? name : "?", deviceSpec.freq, deviceSpec.channels);

    return true;
}


void Recorder::close()
{
    stop();

    if (_stream) {
        SDL_DestroyAudioStream(_stream);
        _stream = nullptr;
    }

    _device = 0;
    _peak = 0.f;
}


bool Recorder::start(const std::filesystem::path& path, std::string& error)
{
    stop();

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), L"wb");
#else
    std::FILE* file = std::fopen(path.c_str(), "wb");
#endif

    if (!file) {
        const std::u8string name = path.u8string();
        error = "Could not create " + std::string(name.begin(), name.end());
        return false;
    }

    {
        std::scoped_lock peaksLock(_peaksMutex);
        _peaks.clear();
        _block = { 0.f, 0.f };
        _blockFrames = 0;
    }

    std::scoped_lock lock(_fileMutex);
    _file = file;
    _frames = 0;
    _framesInHeader = -1;
    writeHeader();
    _recording = true;

    return true;
}


int64_t Recorder::stop()
{
    std::scoped_lock lock(_fileMutex);

    if (!_file) {
        return 0;
    }

    _recording = false;
    writeHeader();
    std::fclose(_file);
    _file = nullptr;

    return _frames.load();
}


void Recorder::takePeaks(std::vector<media::Waveform::Peak>& peaks)
{
    std::scoped_lock lock(_peaksMutex);
    peaks.insert(peaks.end(), _peaks.begin(), _peaks.end());
    _peaks.clear();
}


void Recorder::writeHeader()
{
    // A header up to date about once a second, so that a recording stays
    // readable if the application stops unexpectedly.
    const int64_t frames = _frames.load();

    if (frames == _framesInHeader) {
        return;
    }

    _framesInHeader = frames;

    const uint32_t dataSize = (uint32_t)std::min<int64_t>(frames * BytesPerSample, 0xFFFFFFFFll - HeaderSize);
    uint8_t header[HeaderSize];

    std::memcpy(header, "RIFF", 4);
    put32(header + 4, HeaderSize - 8 + dataSize);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);
    put16(header + 20, 1);                                  // PCM
    put16(header + 22, 1);                                  // Mono
    put32(header + 24, SampleRate);
    put32(header + 28, SampleRate * BytesPerSample);
    put16(header + 32, BytesPerSample);
    put16(header + 34, BytesPerSample * 8);
    std::memcpy(header + 36, "data", 4);
    put32(header + 40, dataSize);

    const long end = std::ftell(_file);
    std::fseek(_file, 0, SEEK_SET);
    std::fwrite(header, 1, HeaderSize, _file);
    std::fseek(_file, std::max<long>(end, HeaderSize), SEEK_SET);
}


void SDLCALL Recorder::callback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int)
{
    Recorder* self = static_cast<Recorder*>(userdata);

    const int count = additionalAmount / (int)sizeof(float);

    if (count <= 0) {
        return;
    }

    self->_samples.resize((size_t)count);
    const int read = SDL_GetAudioStreamData(stream, self->_samples.data(), count * (int)sizeof(float)) / (int)sizeof(float);

    if (read <= 0) {
        return;
    }

    // Level
    float peak = 0.f;

    for (int i = 0; i < read; i++) {
        peak = std::max(peak, std::fabs(self->_samples[i]));
    }

    float previous = self->_peak.load();
    while (peak > previous && !self->_peak.compare_exchange_weak(previous, peak)) {}

    // File
    if (!self->_recording.load()) {
        return;
    }

    // Peaks, for the waveform.
    {
        std::scoped_lock lock(self->_peaksMutex);

        for (int i = 0; i < read; i++) {
            const float sample = self->_samples[i];

            if (self->_blockFrames == 0) {
                self->_block = { sample, sample };
            }
            else {
                self->_block.min = std::min(self->_block.min, sample);
                self->_block.max = std::max(self->_block.max, sample);
            }

            if (++self->_blockFrames == media::Waveform::BaseBlockSize) {
                self->_peaks.push_back(self->_block);
                self->_blockFrames = 0;
            }
        }
    }

    self->_bytes.resize((size_t)read * BytesPerSample);

    for (int i = 0; i < read; i++) {
        const float sample = std::clamp(self->_samples[i], -1.f, 1.f);
        const int32_t value = (int32_t)std::lround(sample * 8388607.f);
        uint8_t* out = self->_bytes.data() + (size_t)i * BytesPerSample;

        out[0] = (uint8_t)value;
        out[1] = (uint8_t)(value >> 8);
        out[2] = (uint8_t)(value >> 16);
    }

    std::scoped_lock lock(self->_fileMutex);

    if (self->_file) {
        std::fwrite(self->_bytes.data(), 1, self->_bytes.size(), self->_file);
        self->_frames += read;

        if (self->_frames.load() - self->_framesInHeader >= SampleRate) {
            self->writeHeader();
        }
    }
}

}
