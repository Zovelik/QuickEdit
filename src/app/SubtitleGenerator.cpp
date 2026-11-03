#include <app/SubtitleGenerator.h>

#ifdef QUICKEDIT_WHISPER
#include <whisper.h>
#endif

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

#include <core/Guard.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <fstream>


namespace {

constexpr const char* ModelUrl = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";

} // namespace


const std::vector<WhisperModel>& whisperModels()
{
    static const std::vector<WhisperModel> models = {
        { "base", "Base", "ggml-base.bin", 148, "Fast, even without a GPU; approximate." },
        { "small", "Small", "ggml-small.bin", 488, "Good balance; slow without a GPU." },
        { "medium", "Medium", "ggml-medium.bin", 1533, "Accurate; wants a GPU." },
        { "large-v3-turbo", "Large v3 Turbo", "ggml-large-v3-turbo.bin", 1624, "The most accurate, fast on a GPU." },
    };

    return models;
}


std::filesystem::path modelFolder()
{
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    return (documents ? std::filesystem::u8path(documents) : std::filesystem::temp_directory_path()) / "QuickEdit" / "Models";
}


bool modelDownloaded(const WhisperModel& model)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(modelFolder() / model.file, ec);
}


bool speechRecognitionAvailable()
{
#ifdef QUICKEDIT_WHISPER
    return true;
#else
    return false;
#endif
}


// ----------------------------------------------------------------------------
// Download
// ----------------------------------------------------------------------------

ModelDownload::~ModelDownload()
{
    _cancel = true;

    if (_thread.joinable()) {
        _thread.join();
    }
}


void ModelDownload::start(const WhisperModel& model)
{
    if (_running) {
        return;
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    _modelId = model.id;
    _cancel = false;
    _progress = 0.f;
    _running = true;

    {
        std::scoped_lock lock(_mutex);
        _error.clear();
    }

    // An error out of the download ends it, the window told why.
    _thread = std::thread([this, url = std::string(ModelUrl) + model.file, path = modelFolder() / model.file]() {
        if (!core::guarded("Model download", [&]() { run(url, path); })) {
            std::scoped_lock lock(_mutex);
            _error = "The download failed.";
            _running = false;
        }
    });
}


std::string ModelDownload::error() const
{
    std::scoped_lock lock(_mutex);
    return _error;
}


void ModelDownload::run(std::string url, std::filesystem::path path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::filesystem::path part = path;
    part += ".part";

    AVIOContext* input = nullptr;
    std::string error;

    if (const int result = avio_open2(&input, url.c_str(), AVIO_FLAG_READ, nullptr, nullptr); result < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(result, text, sizeof(text));
        error = std::string("Could not download the model: ") + text;
    }
    else {
        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        const int64_t size = avio_size(input);
        int64_t received = 0;
        std::vector<unsigned char> buffer(1 << 20);

        while (!_cancel) {
            const int count = avio_read(input, buffer.data(), (int)buffer.size());

            if (count <= 0) {
                break;
            }

            file.write((const char*)buffer.data(), count);
            received += count;

            if (size > 0) {
                _progress = (float)((double)received / (double)size);
            }
        }

        file.close();
        avio_closep(&input);

        if (_cancel) {
            error = "Download cancelled.";
        }
        else if (!file || (size > 0 && received != size) || received < (1 << 20)) {
            error = "The download did not complete.";
        }
        else {
            std::filesystem::rename(part, path, ec);

            if (ec) {
                error = "Could not save the model: " + ec.message();
            }
        }
    }

    if (!error.empty()) {
        std::filesystem::remove(part, ec);
        SDL_Log("Model download: %s", error.c_str());
    }

    {
        std::scoped_lock lock(_mutex);
        _error = error;
    }

    _running = false;
}


// ----------------------------------------------------------------------------
// Generation
// ----------------------------------------------------------------------------

SubtitleGenerator::~SubtitleGenerator()
{
    _cancel = true;

    if (_thread.joinable()) {
        _thread.join();
    }
}


void SubtitleGenerator::start(std::vector<float> samples, const Settings& settings)
{
    if (_running) {
        return;
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    _cancel = false;
    _done = false;
    _progress = 0.f;
    _running = true;

    {
        std::scoped_lock lock(_mutex);
        _cues.clear();
        _error.clear();
    }

    // An error out of the recognition ends it, the window told why.
    _thread = std::thread([this, samples = std::move(samples), settings]() mutable {
        if (!core::guarded("Subtitle generation", [&]() { run(std::move(samples), settings); })) {
            {
                std::scoped_lock lock(_mutex);
                _cues.clear();
                _error = "The recognition failed.";
            }

            _done = true;
            _running = false;
        }
    });
}


std::vector<core::SubtitleCue> SubtitleGenerator::cues() const
{
    std::scoped_lock lock(_mutex);
    return _cues;
}


std::string SubtitleGenerator::error() const
{
    std::scoped_lock lock(_mutex);
    return _error;
}


void SubtitleGenerator::run(std::vector<float> samples, Settings settings)
{
    std::vector<core::SubtitleCue> cues;
    std::string error;

#ifdef QUICKEDIT_WHISPER
    whisper_context_params contextParams = whisper_context_default_params();
    contextParams.use_gpu = settings.useGpu;

    const std::u8string modelName = settings.model.u8string();
    whisper_context* context = whisper_init_from_file_with_params(std::string(modelName.begin(), modelName.end()).c_str(), contextParams);

    if (!context) {
        error = "Could not load the model.";
    }
    else {
        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        params.language = settings.language.c_str();
        params.translate = false;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_special = false;
        params.print_timestamps = false;
        params.n_threads = std::max(1u, std::thread::hardware_concurrency() / 2);

        // Lines of subtitles, cut between words.
        params.token_timestamps = true;
        params.max_len = settings.maxCharacters;
        params.split_on_word = true;

        params.progress_callback = [](whisper_context*, whisper_state*, int progress, void* user) {
            static_cast<SubtitleGenerator*>(user)->_progress = (float)progress / 100.f;
        };
        params.progress_callback_user_data = this;

        params.abort_callback = [](void* user) {
            return static_cast<SubtitleGenerator*>(user)->_cancel.load();
        };
        params.abort_callback_user_data = this;

        if (whisper_full(context, params, samples.data(), (int)samples.size()) != 0) {
            error = _cancel ? "Cancelled." : "The recognition failed.";
        }
        else {
            const int count = whisper_full_n_segments(context);

            for (int i = 0; i < count; i++) {
                std::string text = whisper_full_get_segment_text(context, i);
                const size_t first = text.find_first_not_of(' ');

                if (first == std::string::npos) {
                    continue;
                }

                // Times in hundredths of a second.
                core::SubtitleCue cue;
                cue.start = core::secondsToTime((double)whisper_full_get_segment_t0(context, i) / 100.0);
                cue.end = core::secondsToTime((double)whisper_full_get_segment_t1(context, i) / 100.0);
                cue.text = text.substr(first);

                if (cue.end > cue.start) {
                    cues.push_back(std::move(cue));
                }
            }
        }

        whisper_free(context);
    }
#else
    (void)samples;
    (void)settings;
    error = "This build has no speech recognition (whisper.cpp).";
#endif

    {
        std::scoped_lock lock(_mutex);
        _cues = std::move(cues);
        _error = error;
    }

    _done = true;
    _running = false;
}
