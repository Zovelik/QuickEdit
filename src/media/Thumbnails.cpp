#include <media/Thumbnails.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <algorithm>


using namespace core;


namespace media
{

namespace {

// At most this many pictures per file.
constexpr int MaxThumbnails = 100;


// Decodes the pictures of a strip, one after the other: a key frame near
// each time (fast; close enough for a thumbnail).
void build(std::shared_ptr<ThumbnailStrip> strip, std::filesystem::path path)
{
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    const std::u8string name = path.u8string();
    const std::string fileName(name.begin(), name.end());

    auto finish = [&]() {
        sws_freeContext(sws);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        av_frame_free(&frame);
        av_packet_free(&packet);
    };

    if (avformat_open_input(&format, fileName.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(format, nullptr) < 0) {
        finish();
        return;
    }

    const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (streamIndex < 0) {
        finish();
        return;
    }

    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    decoder = avcodec_alloc_context3(codec);

    if (!codec || avcodec_parameters_to_context(decoder, stream->codecpar) < 0) {
        finish();
        return;
    }

    decoder->thread_count = 2;
    decoder->skip_frame = AVDISCARD_NONKEY;

    if (avcodec_open2(decoder, codec, nullptr) < 0) {
        finish();
        return;
    }

    const int stride = strip->atlasWidth() * 4;
    std::vector<uint8_t> tile((size_t)ThumbnailStrip::TileWidth * ThumbnailStrip::TileHeight * 4);

    for (int i = 0; i < strip->count && !strip->cancel; i++) {
        const Time time = strip->interval * i;
        const int64_t timestamp = av_rescale_q(time, AVRational{ 1, (int)TicksPerSecond }, stream->time_base) +
            (stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0);

        if (i > 0) {
            av_seek_frame(format, streamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(decoder);
        }

        // The first picture from there.
        bool decoded = false;

        while (!decoded && av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == streamIndex && avcodec_send_packet(decoder, packet) >= 0) {
                decoded = avcodec_receive_frame(decoder, frame) >= 0;
            }

            av_packet_unref(packet);
        }

        if (!decoded) {
            // The end of the file: the decoder may hold the last picture.
            avcodec_send_packet(decoder, nullptr);
            decoded = avcodec_receive_frame(decoder, frame) >= 0;
        }

        if (decoded) {
            // Fit in the tile, black around.
            const float fit = std::min((float)ThumbnailStrip::TileWidth / (float)frame->width, (float)ThumbnailStrip::TileHeight / (float)frame->height);
            const int w = std::clamp((int)((float)frame->width * fit), 1, ThumbnailStrip::TileWidth);
            const int h = std::clamp((int)((float)frame->height * fit), 1, ThumbnailStrip::TileHeight);
            const int x = (ThumbnailStrip::TileWidth - w) / 2;
            const int y = (ThumbnailStrip::TileHeight - h) / 2;

            for (size_t p = 0; p < tile.size(); p += 4) {
                tile[p] = tile[p + 1] = tile[p + 2] = 0;
                tile[p + 3] = 255;
            }

            sws = sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);

            if (sws) {
                uint8_t* data[4] = { tile.data() + ((size_t)y * ThumbnailStrip::TileWidth + x) * 4, nullptr, nullptr, nullptr };
                const int linesize[4] = { ThumbnailStrip::TileWidth * 4, 0, 0, 0 };
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, data, linesize);
            }

            av_frame_unref(frame);
        }

        // Into the atlas (a tile left black when nothing could be decoded).
        std::scoped_lock lock(strip->mutex);
        const int column = i % ThumbnailStrip::Columns;
        const int row = i / ThumbnailStrip::Columns;

        for (int line = 0; line < ThumbnailStrip::TileHeight; line++) {
            std::copy_n(tile.data() + (size_t)line * ThumbnailStrip::TileWidth * 4, ThumbnailStrip::TileWidth * 4,
                strip->rgba.data() + (size_t)(row * ThumbnailStrip::TileHeight + line) * stride + (size_t)column * ThumbnailStrip::TileWidth * 4);
        }

        strip->ready = i + 1;
        strip->dirty = true;
    }

    finish();
}

} // namespace


ThumbnailCache::~ThumbnailCache()
{
    std::scoped_lock lock(_mutex);

    for (auto& [path, strip] : _strips) {
        strip->cancel = true;
    }
}


std::shared_ptr<ThumbnailStrip> ThumbnailCache::get(const std::filesystem::path& path, Time duration, bool image)
{
    std::scoped_lock lock(_mutex);

    if (auto it = _strips.find(path); it != _strips.end()) {
        return it->second;
    }

    auto strip = std::make_shared<ThumbnailStrip>();

    if (!image && duration > 0) {
        strip->interval = std::max<Time>(TicksPerSecond, duration / MaxThumbnails);
        strip->count = (int)std::clamp<Time>((duration + strip->interval - 1) / strip->interval, 1, MaxThumbnails);
    }

    strip->rgba.assign((size_t)strip->atlasWidth() * strip->atlasHeight() * 4, 0);
    _strips[path] = strip;

    _pool.submit([strip, path]() { build(strip, path); });

    return strip;
}

}
