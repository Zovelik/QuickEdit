#pragma once

#include <core/Time.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;


namespace media
{
    // A decoded picture. Either on the GPU (hwFrame: an AV_PIX_FMT_VULKAN
    // frame, whose VkImage stays owned by FFmpeg while this object lives), or
    // converted by the CPU to RGBA 8 bits (rgba), for the formats the GPU
    // does not decode and for still images.
    struct VideoFrame
    {
        ~VideoFrame();

        core::Time time = 0;        // In the media
        core::Time duration = 0;

        uint32_t width = 0;
        uint32_t height = 0;

        AVFrame* hwFrame = nullptr;
        std::vector<uint8_t> rgba;

        core::Time end() const { return time + duration; }
        bool covers(core::Time t) const { return t >= time && t < end(); }
    };


    // Decodes the video stream of a file, sequentially, seeking when asked
    // for a time out of reach. Not thread safe: used by one thread at a
    // time (see VideoPlayer).
    class VideoReader
    {
    public:
        // hwDevice: FFmpeg Vulkan device to decode on, nullptr to decode on
        // the CPU. Falls back on the CPU for the codecs the GPU does not
        // decode. Throws std::runtime_error when the file has no video.
        VideoReader(const std::filesystem::path& path, AVBufferRef* hwDevice);
        ~VideoReader();

        VideoReader(const VideoReader&) = delete;
        VideoReader& operator=(const VideoReader&) = delete;

        // Frame shown at time t, nullptr past the end. Decodes forward from
        // the current position when t is close ahead, seeks otherwise.
        std::shared_ptr<VideoFrame> frameAt(core::Time t);

        // Next frame in decoding order, nullptr at the end.
        std::shared_ptr<VideoFrame> nextFrame();

        bool hardware() const { return _hardware; }
        bool stillImage() const { return _stillImage; }

    private:
        void openDecoder(AVBufferRef* hwDevice);
        void seek(core::Time t);

        // Decodes the next frame into _frame. False at the end.
        bool decodeNext();
        std::shared_ptr<VideoFrame> makeFrame();

        AVFormatContext* _format = nullptr;
        AVCodecContext* _codec = nullptr;
        AVPacket* _packet = nullptr;
        AVFrame* _frame = nullptr;
        SwsContext* _sws = nullptr;

        int _streamIndex = -1;
        core::Rational _timeBase = { 1, 1 };
        core::Time _startTime = 0;
        core::Time _frameDuration = 0;
        bool _hardware = false;
        bool _stillImage = false;
        bool _endOfStream = false;

        // Last frame returned, kept for still images and repeated requests.
        std::shared_ptr<VideoFrame> _last;
    };
}
