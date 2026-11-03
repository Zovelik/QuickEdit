#include <app/Exporter.h>

#include <audio/Loudness.h>
#include <audio/Mixer.h>
#include <core/Layers.h>
#include <core/Subtitles.h>
#include <media/VideoReader.h>
#include <render/Compositor.h>
#include <render/FontLibrary.h>
#include <render/Nv12Converter.h>
#include <render/VideoFrameRenderer.h>

#include <vkutils/Buffer.h>
#include <vkutils/util.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>


using namespace core;


namespace {

std::string errorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}


void check(int result, const char* what)
{
    if (result < 0) {
        throw std::runtime_error(std::string(what) + ": " + errorString(result));
    }
}


// Encoders tried in order, for each codec. The Vulkan ones take the frames
// on the GPU; the others take NV12 frames from the CPU.
std::vector<const char*> encoderCandidates(ExportSettings::Codec codec)
{
    switch (codec) {
        case ExportSettings::Codec::H265:
            return { "hevc_vulkan", "hevc_nvenc", "hevc_amf", "hevc_qsv", "hevc_mf" };
        case ExportSettings::Codec::AV1:
            // av1_vulkan of FFmpeg 8.1 produces corrupted streams on NVIDIA
            // (references drift, no keyframes): the dedicated hardware
            // encoders first, for now.
            return { "av1_nvenc", "av1_amf", "av1_qsv", "av1_vulkan", "libsvtav1", "libaom-av1" };
        default:
            return { "h264_vulkan", "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf", "libopenh264" };
    }
}


// The advanced settings in the options of an encoder, each family its own
// names (the others keep their defaults).
void applyEncoderOptions(AVCodecContext* context, const std::string& name, const ExportSettings& settings)
{
    using RateControl = ExportSettings::RateControl;
    using Speed = ExportSettings::Speed;

    auto set = [&](const char* option, const std::string& value) {
        if (av_opt_set(context->priv_data, option, value.c_str(), 0) < 0) {
            SDL_Log("Export: %s has no option %s=%s", name.c_str(), option, value.c_str());
        }
    };

    const RateControl rc = settings.rateControl;
    const std::string quality = std::to_string(std::clamp(settings.quality, 0, 51));

    if (rc == RateControl::ConstantQuality) {
        context->bit_rate = 0;
        context->rc_max_rate = 0;
        context->rc_buffer_size = 0;
    }
    else if (rc == RateControl::CBR) {
        context->rc_max_rate = context->bit_rate;
        context->rc_min_rate = context->bit_rate;
    }

    if (name.find("_vulkan") != std::string::npos) {
        set("rc_mode", rc == RateControl::CBR ? "cbr" : rc == RateControl::ConstantQuality ? "cqp" : "vbr");

        if (rc == RateControl::ConstantQuality) set("qp", quality);
        if (settings.speed == Speed::Best) set("tune", "hq");

        set("usage", "record");
    }
    else if (name.find("_nvenc") != std::string::npos) {
        set("preset", settings.speed == Speed::Fast ? "p2" : settings.speed == Speed::Best ? "p7" : "p5");
        set("tune", "hq");
        set("rc", rc == RateControl::CBR ? "cbr" : "vbr");

        if (rc == RateControl::ConstantQuality) set("cq", quality);
    }
    else if (name.find("_amf") != std::string::npos) {
        set("quality", settings.speed == Speed::Fast ? "speed" : settings.speed == Speed::Best ? "quality" : "balanced");
        set("rc", rc == RateControl::CBR ? "cbr" : rc == RateControl::ConstantQuality ? "cqp" : "vbr_peak");

        if (rc == RateControl::ConstantQuality) {
            set("qp_i", quality);
            set("qp_p", quality);
            set("qp_b", quality);
        }
    }
    else if (name.find("_qsv") != std::string::npos) {
        set("preset", settings.speed == Speed::Fast ? "veryfast" : settings.speed == Speed::Best ? "veryslow" : "medium");

        if (rc == RateControl::ConstantQuality) {
            context->global_quality = std::clamp(settings.quality, 1, 51);
            context->flags |= AV_CODEC_FLAG_QSCALE;
        }
    }
    else if (name.find("_mf") != std::string::npos) {
        set("rate_control", rc == RateControl::CBR ? "cbr" : rc == RateControl::ConstantQuality ? "quality" : "pc_vbr");

        if (rc == RateControl::ConstantQuality) set("quality", std::to_string(std::clamp(100 - 2 * settings.quality, 0, 100)));
    }
    else if (name == "libsvtav1") {
        set("preset", settings.speed == Speed::Fast ? "10" : settings.speed == Speed::Best ? "4" : "7");

        if (rc == RateControl::ConstantQuality) set("crf", std::to_string(std::clamp(settings.quality + 10, 0, 63)));
    }
    else if (name == "libaom-av1") {
        set("cpu-used", settings.speed == Speed::Fast ? "8" : settings.speed == Speed::Best ? "3" : "6");

        if (rc == RateControl::ConstantQuality) set("crf", std::to_string(std::clamp(settings.quality + 10, 0, 63)));
    }
}


// FFmpeg objects of an export, freed whatever happens.
struct Encoding
{
    AVFormatContext* format = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
    AVBufferRef* frames = nullptr;          // Vulkan frames of the encoder
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    bool fileOpen = false;

    ~Encoding()
    {
        sws_freeContext(sws);
        av_packet_free(&packet);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        av_buffer_unref(&frames);

        if (format) {
            if (fileOpen) {
                avio_closep(&format->pb);
            }

            avformat_free_context(format);
        }
    }

    // Writes the packets the encoder has ready.
    void drain(AVCodecContext* codec, AVStream* stream)
    {
        while (true) {
            const int result = avcodec_receive_packet(codec, packet);

            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }

            check(result, "Encoding failed");

            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            check(av_interleaved_write_frame(format, packet), "Could not write the file");
        }
    }
};


// GPU objects of an export, on the export thread.
struct GpuWork
{
    const GPU::Device& device;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;       // Submitted, not waited for

    GpuWork(const GPU::Device& d, uint32_t family)
        : device(d)
    {
        VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        VK_THROW_IF_FAILED(vkCreateCommandPool(device.device(), &poolInfo, nullptr, &pool));

        VkCommandBufferAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = pool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VK_THROW_IF_FAILED(vkAllocateCommandBuffers(device.device(), &allocateInfo, &commandBuffer));

        VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_THROW_IF_FAILED(vkCreateFence(device.device(), &fenceInfo, nullptr, &fence));
    }

    ~GpuWork()
    {
        if (pending) {
            vkWaitForFences(device.device(), 1, &fence, VK_TRUE, UINT64_MAX);
        }

        vkDestroyFence(device.device(), fence, nullptr);
        vkDestroyCommandPool(device.device(), pool, nullptr);
    }
};

} // namespace


Exporter::Exporter(ExportContext context, Project project,
    std::map<std::pair<Id, int>, std::shared_ptr<const media::AudioBuffer>> audio,
    std::map<Id, std::vector<std::shared_ptr<audio::AudioEffect>>> effects,
    ExportSettings settings)
    : _context(std::move(context))
    , _project(std::move(project))
    , _audio(std::move(audio))
    , _effects(std::move(effects))
    , _settings(std::move(settings))
{
    _thread = std::thread([this]() { run(); });
}


Exporter::~Exporter()
{
    _cancel = true;
    _thread.join();
}


std::string Exporter::error() const
{
    std::scoped_lock lock(_mutex);
    return _error;
}


std::string Exporter::encoder() const
{
    std::scoped_lock lock(_mutex);
    return _encoder;
}


std::string Exporter::loudnessReport() const
{
    std::scoped_lock lock(_mutex);
    return _loudnessReport;
}


void Exporter::run()
{
    try {
        exportVideo();
        _succeeded = !_cancel;
    }
    catch (const std::exception& e) {
        std::scoped_lock lock(_mutex);
        _error = e.what();
    }

    // A cancelled or failed export leaves no partial file.
    if (!_succeeded) {
        std::error_code error;
        std::filesystem::remove(_settings.path, error);
    }

    _done = true;
}


void Exporter::exportVideo()
{
    const Sequence& sequence = _project.sequence();
    const GPU::Device& device = *_context.device;

    // The size and rate asked, else the ones of the sequence; the width from
    // the shape of the sequence, both even for the encoders.
    const uint32_t height = (_settings.height ? _settings.height : sequence.height) & ~1u;
    const uint32_t width = (uint32_t)std::lround((double)height * sequence.width / std::max(1u, sequence.height)) & ~1u;
    const Rational fps = (_settings.frameRate.num > 0) ? _settings.frameRate : sequence.frameRate;
    const Time duration = sequence.duration();
    const int64_t frameCount = std::max<int64_t>(1, timeToFrame(duration - 1, fps) + 1);

    if (duration <= 0) {
        throw std::runtime_error("The sequence is empty");
    }

    // ------------------------------------------------------------------------
    // Muxer and encoders
    // ------------------------------------------------------------------------

    Encoding encoding;
    encoding.packet = av_packet_alloc();

    const std::u8string u8 = _settings.path.u8string();
    const std::string path(u8.begin(), u8.end());

    check(avformat_alloc_output_context2(&encoding.format, nullptr, nullptr, path.c_str()), "Unsupported file type");

    const bool globalHeader = (encoding.format->oformat->flags & AVFMT_GLOBALHEADER) != 0;
    const int64_t bitrate = (int64_t)(_settings.bitrateMbps * 1e6);
    bool vulkanFrames = false;

    for (const char* name : encoderCandidates(_settings.codec)) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        const bool vulkan = std::strstr(name, "_vulkan") != nullptr;

        if (!codec || (vulkan && (!_context.vulkanEncode || !_context.hwDevice))) {
            continue;
        }

        AVCodecContext* context = avcodec_alloc_context3(codec);
        context->width = (int)width;
        context->height = (int)height;
        context->time_base = { (int)fps.den, (int)fps.num };
        context->framerate = { (int)fps.num, (int)fps.den };
        context->sample_aspect_ratio = { 1, 1 };
        context->bit_rate = bitrate;
        context->rc_max_rate = bitrate * 3 / 2;
        context->rc_buffer_size = (int)std::min<int64_t>(bitrate * 2, INT32_MAX);
        context->gop_size = std::max(1, (int)std::lround(std::clamp(_settings.keyframeSeconds, 0.1f, 20.f) * fps.toDouble()));

        if (_settings.bFrames >= 0) {
            context->max_b_frames = std::clamp(_settings.bFrames, 0, 4);
        }
        context->color_range = AVCOL_RANGE_MPEG;
        context->colorspace = AVCOL_SPC_BT709;
        context->color_primaries = AVCOL_PRI_BT709;
        context->color_trc = AVCOL_TRC_BT709;

        if (globalHeader) {
            context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        AVBufferRef* frames = nullptr;

        if (vulkan) {
            frames = av_hwframe_ctx_alloc(_context.hwDevice);
            AVHWFramesContext* framesContext = (AVHWFramesContext*)frames->data;
            framesContext->format = AV_PIX_FMT_VULKAN;
            framesContext->sw_format = AV_PIX_FMT_NV12;
            framesContext->width = (int)width;
            framesContext->height = (int)height;

            if (av_hwframe_ctx_init(frames) < 0) {
                av_buffer_unref(&frames);
                avcodec_free_context(&context);
                continue;
            }

            context->hw_frames_ctx = av_buffer_ref(frames);
            context->pix_fmt = AV_PIX_FMT_VULKAN;
            context->sw_pix_fmt = AV_PIX_FMT_NV12;
        }
        else {
            // NV12 when the encoder takes it, its first format otherwise.
            const void* formats = nullptr;
            int count = 0;
            avcodec_get_supported_config(context, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &formats, &count);

            context->pix_fmt = AV_PIX_FMT_NV12;

            if (formats && count > 0) {
                const AVPixelFormat* list = (const AVPixelFormat*)formats;
                context->pix_fmt = std::find(list, list + count, AV_PIX_FMT_NV12) != list + count ? AV_PIX_FMT_NV12 : list[0];
            }
        }

        applyEncoderOptions(context, name, _settings);

        if (avcodec_open2(context, codec, nullptr) < 0) {
            SDL_Log("Export: encoder %s unavailable", name);
            av_buffer_unref(&frames);
            avcodec_free_context(&context);
            continue;
        }

        encoding.video = context;
        encoding.frames = frames;
        vulkanFrames = vulkan;

        std::scoped_lock lock(_mutex);
        _encoder = name;
        break;
    }

    if (!encoding.video) {
        throw std::runtime_error("No video encoder available for this codec");
    }

    SDL_Log("Export: video encoder %s", encoder().c_str());

    encoding.videoStream = avformat_new_stream(encoding.format, nullptr);
    check(avcodec_parameters_from_context(encoding.videoStream->codecpar, encoding.video), "Video stream");
    encoding.videoStream->time_base = encoding.video->time_base;
    encoding.videoStream->avg_frame_rate = encoding.video->framerate;

    if (_settings.audio) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);

        if (!codec) {
            throw std::runtime_error("No AAC encoder");
        }

        encoding.audio = avcodec_alloc_context3(codec);
        encoding.audio->sample_rate = (int)sequence.sampleRate;
        encoding.audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
        encoding.audio->bit_rate = (int64_t)std::clamp(_settings.audioKbps, 64, 512) * 1000;
        encoding.audio->time_base = { 1, (int)sequence.sampleRate };
        av_channel_layout_default(&encoding.audio->ch_layout, 2);

        if (globalHeader) {
            encoding.audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        check(avcodec_open2(encoding.audio, codec, nullptr), "Could not open the audio encoder");

        encoding.audioStream = avformat_new_stream(encoding.format, nullptr);
        check(avcodec_parameters_from_context(encoding.audioStream->codecpar, encoding.audio), "Audio stream");
        encoding.audioStream->time_base = encoding.audio->time_base;
    }

    if (!(encoding.format->oformat->flags & AVFMT_NOFILE)) {
        check(avio_open(&encoding.format->pb, path.c_str(), AVIO_FLAG_WRITE), "Could not create the file");
        encoding.fileOpen = true;
    }

    check(avformat_write_header(encoding.format, nullptr), "Could not write the file header");

    // ------------------------------------------------------------------------
    // GPU: the same composition as in the viewer
    // ------------------------------------------------------------------------

    GpuWork gpu(device, _context.queueFamily);
    FontLibrary fonts(_context.defaultFont, false);
    Compositor compositor(device, *_context.pipelineCache, fonts, false);
    Nv12Converter converter(device, *_context.pipelineCache);

    // Decoders by layer key (track, slot and nesting, see LayerRequest):
    // a renderer each, a reader per media, decoding forward.
    std::map<uint64_t, std::unique_ptr<VideoFrameRenderer>> renderers;
    std::map<uint64_t, std::map<Id, std::unique_ptr<media::VideoReader>>> readers;

    // CPU encoders: the frame is read back.
    std::unique_ptr<GPU::Buffer> readback;
    AVFrame* softwareFrame = nullptr;

    if (!vulkanFrames) {
        readback = std::make_unique<GPU::Buffer>(
            "Export readback", device, (VkDeviceSize)width * height * 4,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        readback->bind();

        encoding.sws = sws_alloc_context();
        encoding.sws->threads = 0;
        encoding.sws->flags = SWS_BILINEAR;
    }

    // Audio
    std::shared_ptr<const audio::MixGraph> mixGraph = audio::buildMixGraph(_project,
        [this](Id mediaId, int stream) {
            const auto it = _audio.find({ mediaId, stream });
            return (it != _audio.end()) ? it->second : nullptr;
        },
        [this](Id owner, const std::vector<EffectSlot>&) {
            const auto it = _effects.find(owner);
            return (it != _effects.end()) ? it->second : std::vector<std::shared_ptr<audio::AudioEffect>>();
        });

    // Without effects: the sound levels given to the shaders, frame by frame.
    std::shared_ptr<const audio::MixGraph> levelsGraph = audio::buildMixGraph(_project,
        [this](Id mediaId, int stream) {
            const auto it = _audio.find({ mediaId, stream });
            return (it != _audio.end()) ? it->second : nullptr;
        });

    AVFrame* audioFrame = av_frame_alloc();
    std::vector<float> mixBuffer;
    int64_t audioPosition = 0;
    const int64_t audioEnd = mulDivFloor(duration, sequence.sampleRate, TicksPerSecond);

    // Loudness normalization: the whole mix first, measured and kept in a
    // file (the effects process it once), then read back with the gain.
    std::filesystem::path mixPath;
    std::ifstream mixFile;
    float mixGain = 1.f;

    if (encoding.audio && _settings.normalizeLoudness) {
        mixPath = std::filesystem::temp_directory_path() / ("QuickEdit mix " + std::to_string(SDL_GetTicksNS()) + ".raw");
        std::ofstream out(mixPath, std::ios::binary);
        audio::LoudnessMeter meter(sequence.sampleRate);
        constexpr int64_t Chunk = 4096;

        for (int64_t position = 0; position < audioEnd && !_cancel; position += Chunk) {
            const int64_t count = std::min(Chunk, audioEnd - position);
            mixBuffer.resize((size_t)count * 2);
            audio::mix(*mixGraph, position, count, mixBuffer.data());
            meter.add(mixBuffer.data(), count);
            out.write((const char*)mixBuffer.data(), (std::streamsize)(mixBuffer.size() * sizeof(float)));
        }

        out.close();

        if (!out) {
            std::filesystem::remove(mixPath);
            throw std::runtime_error("Could not write the sound to measure its loudness (disk full?)");
        }

        const double lufs = meter.integratedLufs();
        const double gainDb = audio::normalizationGainDb(lufs, meter.peak(), _settings.targetLufs);
        mixGain = (float)std::pow(10.0, gainDb / 20.0);
        mixFile.open(mixPath, std::ios::binary);

        char report[160];

        if (lufs < audio::LoudnessMeter::Silence) {
            std::snprintf(report, sizeof(report), "Silent sound: left as it is.");
        }
        else {
            std::snprintf(report, sizeof(report), "Sound: %.1f LUFS, %+.1f dB applied (%.1f LUFS)%s.", lufs, gainDb, lufs + gainDb,
                lufs + gainDb < _settings.targetLufs - 0.1 ? ", held back by the peaks" : "");
        }

        SDL_Log("Export: %s", report);
        std::scoped_lock lock(_mutex);
        _loudnessReport = report;
    }

    // The file of the mix goes with the export.
    struct RemoveFile
    {
        std::ifstream& file;
        std::filesystem::path path;

        ~RemoveFile()
        {
            file.close();

            if (!path.empty()) {
                std::error_code error;
                std::filesystem::remove(path, error);
            }
        }
    } removeMix{ mixFile, mixPath };

    auto encodeAudioUntil = [&](int64_t until) {
        if (!encoding.audio) {
            return;
        }

        const int frameSize = (encoding.audio->frame_size > 0) ? encoding.audio->frame_size : 1024;

        while (audioPosition < std::min(until, audioEnd)) {
            const int count = (int)std::min<int64_t>(frameSize, audioEnd - audioPosition);

            // The last frame may be shorter, unless a full one is still
            // ahead of the video.
            if (count < frameSize && until < audioEnd) {
                return;
            }

            mixBuffer.resize((size_t)count * 2);

            if (mixFile.is_open()) {
                mixFile.read((char*)mixBuffer.data(), (std::streamsize)(mixBuffer.size() * sizeof(float)));

                for (float& sample : mixBuffer) {
                    sample = std::clamp(sample * mixGain, -1.f, 1.f);
                }
            }
            else {
                audio::mix(*mixGraph, audioPosition, count, mixBuffer.data());
            }

            av_frame_unref(audioFrame);
            audioFrame->nb_samples = count;
            audioFrame->format = AV_SAMPLE_FMT_FLTP;
            audioFrame->sample_rate = encoding.audio->sample_rate;
            av_channel_layout_copy(&audioFrame->ch_layout, &encoding.audio->ch_layout);
            check(av_frame_get_buffer(audioFrame, 0), "Audio frame");

            float* left = (float*)audioFrame->data[0];
            float* right = (float*)audioFrame->data[1];

            for (int s = 0; s < count; s++) {
                left[s] = mixBuffer[2 * s];
                right[s] = mixBuffer[2 * s + 1];
            }

            audioFrame->pts = audioPosition;
            check(avcodec_send_frame(encoding.audio, audioFrame), "Audio encoding failed");
            encoding.drain(encoding.audio, encoding.audioStream);

            audioPosition += count;
        }
    };

    // ------------------------------------------------------------------------
    // Frames
    // ------------------------------------------------------------------------

    for (int64_t frameIndex = 0; frameIndex < frameCount && !_cancel; frameIndex++) {
        const Time t = frameToTime(frameIndex, fps);

        compositor.beginFrame();
        converter.beginFrame();

        for (auto& [key, renderer] : renderers) {
            renderer->beginFrame();
        }

        const VkCommandBufferBeginInfo beginInfo = {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr
        };

        VK_THROW_IF_FAILED(vkResetCommandBuffer(gpu.commandBuffer, 0));
        VK_THROW_IF_FAILED(vkBeginCommandBuffer(gpu.commandBuffer, &beginInfo));

        VideoFrameRenderer::Sync sync;

        // The layers of a level (the sequence, or a nested one).
        std::function<std::vector<Compositor::Layer>(const std::vector<LayerRequest>&, const Sequence&)> build;

        build = [&](const std::vector<LayerRequest>& requests, const Sequence& level) {
            std::vector<Compositor::Layer> layers;

            for (const LayerRequest& request : requests) {
                const Clip& clip = *request.clip;

                Compositor::Layer layer = { &clip, request.localTime, nullptr, 0, 0,
                    request.opacity, request.offsetX, request.offsetY, request.scale, request.wipe };
                layer.trackEffects = &level.videoTracks[request.track].videoEffects;
                layer.trackBlend = level.videoTracks[request.track].blend;
                layer.transitionRole = request.transitionRole;
                layer.transitionProgress = request.transitionProgress;
                layer.transitionShader = request.transition ? _project.findShader(request.transition->shader) : nullptr;
                
                // Blend and sampling shaders: the clip's blend first (a mode of its
                // own goes before the shader of its track).
                const Track& owner = level.videoTracks[request.track];
                const std::string& blendId = !clip.blendShader.empty() ? clip.blendShader : (clip.blend == BlendMode::Normal) ? owner.blendShader : clip.blendShader;
                const ShaderDef* blendShader = blendId.empty() ? nullptr : _project.findShader(blendId);
                const ShaderDef* filterShader = clip.filterShader.empty() ? nullptr : _project.findShader(clip.filterShader);
                layer.blendShader = (blendShader && blendShader->kind == ShaderKind::Blend) ? blendShader : nullptr;
                layer.filterShader = (filterShader && filterShader->kind == ShaderKind::Resample) ? filterShader : nullptr;

                // Generators: the shader makes the picture.
                if (clip.kind == ClipKind::Generator) {
                    layer.generator = _project.findShader(clip.shader);

                    if (layer.generator) {
                        layers.push_back(layer);
                    }

                    continue;
                }

                if (clip.kind == ClipKind::Text) {
                    layers.push_back(layer);
                    continue;
                }

                // A nested sequence: its own layers, composited first.
                if (clip.kind == ClipKind::Sequence) {
                    if (request.nested) {
                        layer.children = build(request.children, *request.nested);
                        layer.childWidth = request.nested->width;
                        layer.childHeight = request.nested->height;
                        layers.push_back(std::move(layer));
                    }

                    continue;
                }

                const MediaItem* media = _project.findMedia(clip.mediaId);

                if (!media || media->type == MediaType::Audio) {
                    continue;
                }

                std::unique_ptr<media::VideoReader>& reader = readers[request.key][media->id];

                if (!reader) {
                    try {
                        reader = std::make_unique<media::VideoReader>(media->path, _context.hwDevice);
                    }
                    catch (const std::exception& e) {
                        throw std::runtime_error(media->name() + ": " + e.what());
                    }
                }

                std::unique_ptr<VideoFrameRenderer>& renderer = renderers[request.key];

                if (!renderer) {
                    renderer = std::make_unique<VideoFrameRenderer>(device, *_context.pipelineCache);
                    renderer->beginFrame();
                }

                const std::shared_ptr<media::VideoFrame> frame = reader->frameAt(request.sourceTime);

                if (frame && renderer->cmdRender(gpu.commandBuffer, frame, sync) && renderer->image()) {
                    layer.image = renderer->image();
                    layer.width = renderer->width();
                    layer.height = renderer->height();
                    layers.push_back(layer);
                }
            }

            return layers;
        };

        std::vector<Compositor::Layer> layers = build(layersAt(sequence, t, [this](Id id) { return _project.findSequence(id); }), sequence);

        // The subtitles, over everything.
        const std::vector<Clip> subtitles = subtitleClipsAt(sequence, t);

        for (const Clip& clip : subtitles) {
            layers.push_back({ &clip, t - clip.start, nullptr, 0, 0, 1.f, 0.f, 0.f, 1.f, 1.f });
        }

        const audio::SoundLevels sound = audio::soundLevels(*levelsGraph, mulDivFloor(t, levelsGraph->sampleRate, TicksPerSecond));
        compositor.setSound(sound.level, sound.bass, sound.mid, sound.treble);
        compositor.cmdComposite(gpu.commandBuffer, width, height, layers, sequence.width, sequence.height);

        AVFrame* videoFrame = av_frame_alloc();

        if (vulkanFrames) {
            const int result = av_hwframe_get_buffer(encoding.frames, videoFrame, 0);

            if (result < 0) {
                av_frame_free(&videoFrame);
                check(result, "Could not get an encoder frame");
            }

            if (!converter.cmdConvert(gpu.commandBuffer, *compositor.output(), videoFrame, sync)) {
                av_frame_free(&videoFrame);
                throw std::runtime_error("Could not convert the frame for the encoder");
            }
        }
        else {
            GPU::Image& output = *compositor.output();
            output.cmdBarrierTransition(gpu.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            VkBufferImageCopy region = {};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { width, height, 1 };
            vkCmdCopyImageToBuffer(gpu.commandBuffer, output.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer(), 1, &region);

            output.cmdBarrierTransition(gpu.commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            VkMemoryBarrier hostBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(gpu.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
        }

        VK_THROW_IF_FAILED(vkEndCommandBuffer(gpu.commandBuffer));

        // Submit, with the semaphores of the decoded and encoder frames.
        std::vector<VkSemaphore> waitSemaphores;
        std::vector<uint64_t> waitValues;
        std::vector<VkPipelineStageFlags> waitStages;
        std::vector<VkSemaphore> signalSemaphores;
        std::vector<uint64_t> signalValues;

        for (const VideoFrameRenderer::SemaphoreOp& wait : sync.waits) {
            waitSemaphores.push_back(wait.semaphore);
            waitValues.push_back(wait.value);
            waitStages.push_back(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }

        for (const VideoFrameRenderer::SemaphoreOp& signal : sync.signals) {
            signalSemaphores.push_back(signal.semaphore);
            signalValues.push_back(signal.value);
        }

        const VkTimelineSemaphoreSubmitInfo timelineInfo = {
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr,
            (uint32_t)waitValues.size(), waitValues.data(),
            (uint32_t)signalValues.size(), signalValues.data()
        };

        const VkSubmitInfo submitInfo = {
            VK_STRUCTURE_TYPE_SUBMIT_INFO, &timelineInfo,
            (uint32_t)waitSemaphores.size(), waitSemaphores.data(), waitStages.data(),
            1, &gpu.commandBuffer,
            (uint32_t)signalSemaphores.size(), signalSemaphores.data()
        };

        VK_THROW_IF_FAILED(vkResetFences(device.device(), 1, &gpu.fence));

        {
            std::scoped_lock lock(*_context.queueMutex);
            VK_THROW_IF_FAILED(vkQueueSubmit(_context.queue, 1, &submitInfo, gpu.fence));
            gpu.pending = true;
        }

        VK_THROW_IF_FAILED(vkWaitForFences(device.device(), 1, &gpu.fence, VK_TRUE, UINT64_MAX));
        gpu.pending = false;

        // Encode
        if (!vulkanFrames) {
            if (!softwareFrame) {
                softwareFrame = av_frame_alloc();
                softwareFrame->format = encoding.video->pix_fmt;
                softwareFrame->width = (int)width;
                softwareFrame->height = (int)height;
                softwareFrame->color_range = AVCOL_RANGE_MPEG;
                softwareFrame->colorspace = AVCOL_SPC_BT709;
                check(av_frame_get_buffer(softwareFrame, 0), "Video frame");
            }

            check(av_frame_make_writable(softwareFrame), "Video frame");

            AVFrame* rgba = av_frame_alloc();
            rgba->format = AV_PIX_FMT_RGBA;
            rgba->width = (int)width;
            rgba->height = (int)height;
            rgba->data[0] = readback->mappedMemory();
            rgba->linesize[0] = (int)width * 4;

            const int scaled = sws_scale_frame(encoding.sws, softwareFrame, rgba);

            rgba->data[0] = nullptr;
            av_frame_free(&rgba);
            check(scaled, "Color conversion failed");

            av_frame_unref(videoFrame);
            av_frame_ref(videoFrame, softwareFrame);
        }

        videoFrame->pts = frameIndex;
        const int sent = avcodec_send_frame(encoding.video, videoFrame);
        av_frame_free(&videoFrame);
        check(sent, "Video encoding failed");

        encoding.drain(encoding.video, encoding.videoStream);

        // Audio up to the end of this frame.
        encodeAudioUntil(mulDivFloor(frameToTime(frameIndex + 1, fps), sequence.sampleRate, TicksPerSecond));

        _progress = (float)(frameIndex + 1) / (float)frameCount;
    }

    av_frame_free(&softwareFrame);

    if (_cancel) {
        av_frame_free(&audioFrame);
        return;
    }

    // Flush
    encodeAudioUntil(audioEnd);
    av_frame_free(&audioFrame);

    avcodec_send_frame(encoding.video, nullptr);
    encoding.drain(encoding.video, encoding.videoStream);

    if (encoding.audio) {
        avcodec_send_frame(encoding.audio, nullptr);
        encoding.drain(encoding.audio, encoding.audioStream);
    }

    check(av_write_trailer(encoding.format), "Could not finish the file");
}
