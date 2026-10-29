#include <media/MediaFile.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>


namespace media
{

namespace {

// RAII for the FFmpeg objects.
struct FormatContextDeleter { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
struct CodecContextDeleter  { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct PacketDeleter        { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct FrameDeleter         { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct SwrDeleter           { void operator()(SwrContext* p) const { swr_free(&p); } };

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr  = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr        = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr         = std::unique_ptr<AVFrame, FrameDeleter>;
using SwrPtr           = std::unique_ptr<SwrContext, SwrDeleter>;


std::string errorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}


FormatContextPtr openInput(const std::filesystem::path& path)
{
    // FFmpeg takes UTF-8 paths on every platform.
    const std::u8string u8 = path.u8string();
    const std::string utf8(u8.begin(), u8.end());

    AVFormatContext* context = nullptr;
    int result = avformat_open_input(&context, utf8.c_str(), nullptr, nullptr);

    if (result < 0) {
        throw std::runtime_error("Could not open the file: " + errorString(result));
    }

    FormatContextPtr format(context);
    result = avformat_find_stream_info(format.get(), nullptr);

    if (result < 0) {
        throw std::runtime_error("Could not read the streams: " + errorString(result));
    }

    return format;
}


core::Time toTime(int64_t value, AVRational timeBase)
{
    return av_rescale_q(value, timeBase, AVRational{ 1, (int)core::TicksPerSecond });
}


bool isImageFormat(const AVFormatContext* format)
{
    const char* name = format->iformat ? format->iformat->name : "";

    // "image2" for numbered files, "png_pipe", "jpeg_pipe"... for single ones.
    return std::strstr(name, "image2") != nullptr || std::strstr(name, "_pipe") != nullptr;
}

} // namespace


ProbeResult probe(const std::filesystem::path& path)
{
    FormatContextPtr format = openInput(path);
    ProbeResult result;

    const int videoIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // Cover art of audio files is a video stream too: not a video.
    if (videoIndex >= 0 && !(format->streams[videoIndex]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        const AVStream* stream = format->streams[videoIndex];
        const AVCodecParameters* codec = stream->codecpar;

        result.hasVideo = true;
        result.width = (uint32_t)codec->width;
        result.height = (uint32_t)codec->height;
        result.videoCodec = avcodec_get_name(codec->codec_id);

        AVRational rate = stream->avg_frame_rate;

        if (rate.num <= 0 || rate.den <= 0) {
            rate = stream->r_frame_rate;
        }

        if (rate.num > 0 && rate.den > 0) {
            result.frameRate = { rate.num, rate.den };
        }

        result.isImage = isImageFormat(format.get()) || stream->nb_frames == 1;
    }

    if (audioIndex >= 0) {
        const AVCodecParameters* codec = format->streams[audioIndex]->codecpar;

        result.hasAudio = true;
        result.sampleRate = (uint32_t)codec->sample_rate;
        result.channels = (uint32_t)codec->ch_layout.nb_channels;
        result.audioCodec = avcodec_get_name(codec->codec_id);
    }

    if (!result.hasVideo && !result.hasAudio) {
        throw std::runtime_error("The file has no video or audio stream");
    }

    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        result.duration = toTime(format->duration, AV_TIME_BASE_Q);
    }
    else {
        // Fall back on the longest stream.
        for (unsigned int i = 0; i < format->nb_streams; i++) {
            const AVStream* stream = format->streams[i];

            if (stream->duration != AV_NOPTS_VALUE) {
                result.duration = std::max(result.duration, toTime(stream->duration, stream->time_base));
            }
        }
    }

    return result;
}


AudioBuffer decodeAudio(
    const std::filesystem::path& path,
    uint32_t sampleRate,
    const std::atomic<bool>& cancel,
    const std::function<void(float)>& progress)
{
    FormatContextPtr format = openInput(path);

    const AVCodec* decoder = nullptr;
    const int streamIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);

    if (streamIndex < 0 || !decoder) {
        throw std::runtime_error("The file has no readable audio stream");
    }

    // Only the audio packets are needed.
    for (unsigned int i = 0; i < format->nb_streams; i++) {
        format->streams[i]->discard = ((int)i == streamIndex) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }

    const AVStream* stream = format->streams[streamIndex];

    CodecContextPtr codec(avcodec_alloc_context3(decoder));
    avcodec_parameters_to_context(codec.get(), stream->codecpar);

    // As many threads as the decoder can use.
    codec->thread_count = 0;

    int result = avcodec_open2(codec.get(), decoder, nullptr);

    if (result < 0) {
        throw std::runtime_error("Could not open the audio decoder: " + errorString(result));
    }

    AudioBuffer buffer;
    buffer.sampleRate = sampleRate;

    const double durationSeconds = (format->duration != AV_NOPTS_VALUE) ? (double)format->duration / AV_TIME_BASE : 0.0;

    if (durationSeconds > 0.0) {
        buffer.samples.reserve((size_t)((durationSeconds + 1.0) * sampleRate) * AudioBuffer::Channels);
    }

    // The resampler is set up from the first frame: the parameters of the
    // stream may be incomplete until then.
    SwrPtr resampler;
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, AudioBuffer::Channels);

    auto convert = [&](const uint8_t** input, int inputFrames) {
        const int outputFrames = swr_get_out_samples(resampler.get(), inputFrames);

        if (outputFrames <= 0) {
            return;
        }

        const size_t offset = buffer.samples.size();
        buffer.samples.resize(offset + (size_t)outputFrames * AudioBuffer::Channels);

        uint8_t* output = (uint8_t*)(buffer.samples.data() + offset);
        const int converted = swr_convert(resampler.get(), &output, outputFrames, input, inputFrames);

        buffer.samples.resize(offset + (size_t)std::max(0, converted) * AudioBuffer::Channels);
    };

    auto receiveFrames = [&](AVFrame* frame) {
        while (avcodec_receive_frame(codec.get(), frame) >= 0) {
            if (!resampler) {
                AVChannelLayout layout;

                if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
                    av_channel_layout_default(&layout, frame->ch_layout.nb_channels);
                }
                else {
                    av_channel_layout_copy(&layout, &frame->ch_layout);
                }

                SwrContext* swr = nullptr;
                result = swr_alloc_set_opts2(
                    &swr,
                    &stereo, AV_SAMPLE_FMT_FLT, (int)sampleRate,
                    &layout, (AVSampleFormat)frame->format, frame->sample_rate,
                    0, nullptr);

                av_channel_layout_uninit(&layout);

                if (result < 0 || swr_init(swr) < 0) {
                    swr_free(&swr);
                    throw std::runtime_error("Could not convert the audio of the file");
                }

                resampler.reset(swr);
            }

            convert((const uint8_t**)frame->extended_data, frame->nb_samples);
            av_frame_unref(frame);
        }
    };

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());

    while (!cancel && av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == streamIndex) {
            if (progress && durationSeconds > 0.0 && packet->pts != AV_NOPTS_VALUE) {
                const double seconds = (double)packet->pts * av_q2d(stream->time_base);
                progress((float)std::clamp(seconds / durationSeconds, 0.0, 1.0));
            }

            // A corrupt packet is skipped, as players do.
            if (avcodec_send_packet(codec.get(), packet.get()) >= 0) {
                receiveFrames(frame.get());
            }
        }

        av_packet_unref(packet.get());
    }

    // Drain the decoder and the resampler.
    avcodec_send_packet(codec.get(), nullptr);
    receiveFrames(frame.get());

    if (resampler) {
        convert(nullptr, 0);
    }

    buffer.samples.shrink_to_fit();

    if (progress) {
        progress(1.f);
    }

    return buffer;
}

}
