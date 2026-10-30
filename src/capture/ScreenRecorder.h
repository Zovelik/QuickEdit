#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


struct AVFormatContext;


namespace capture
{
    // A way a camera can give its pictures: size, encoding, frame rates.
    struct CameraFormat
    {
        int width = 0;              // 0: what the camera gives by default
        int height = 0;
        std::string codec;          // FFmpeg decoder: "mjpeg", "h264"; empty for raw pictures
        std::string pixelFormat;    // Raw pictures: FFmpeg pixel format ("yuyv422", "nv12"...)

        // The frame rates offered, highest first.
        std::vector<double> frameRates;

        bool automatic() const { return width == 0; }

        // "1920 x 1080  MJPEG"
        std::string label() const;

        bool sameMode(const CameraFormat& other) const
        {
            return width == other.width && height == other.height && codec == other.codec && pixelFormat == other.pixelFormat;
        }
    };

    // A screen, a window or a camera that can be recorded.
    struct CaptureSource
    {
        enum class Kind
        {
            Monitor,
            Window,
            Camera
        };

        Kind kind = Kind::Monitor;
        uint64_t handle = 0;        // HMONITOR or HWND; for a camera, its number from 1
        std::string device;         // Camera: its DirectShow name
        std::string name;

        // Camera: the format asked for, and its frame rate (0: the
        // camera's choice).
        CameraFormat format;
        double frameRate = 0.0;

        bool operator==(const CaptureSource& other) const
        {
            return kind == other.kind && handle == other.handle && device == other.device &&
                format.sameMode(other.format) && frameRate == other.frameRate;
        }

        bool sameDevice(const CaptureSource& other) const { return kind == other.kind && handle == other.handle && device == other.device; }
    };

    // The screens, the cameras, then the windows of the other applications.
    std::vector<CaptureSource> listSources();

    // The formats a camera offers (DirectShow capabilities), the largest
    // first. Empty when unknown (other systems).
    std::vector<CameraFormat> listCameraFormats(const CaptureSource& camera);

    // Keeps the windows of the calling thread out of the captures (or puts
    // them back), so that QuickEdit does not record itself.
    void excludeFromCapture(bool exclude);


    // Live picture of a source, a few times a second, for choosing it:
    // captured and scaled on a thread of its own, letterboxed in Width x
    // Height RGBA pixels.
    class SourcePreview
    {
    public:
        static constexpr int Width = 480;
        static constexpr int Height = 270;

        ~SourcePreview() { stop(); }

        // Shows another source (none: handle 0).
        void show(const CaptureSource& source);

        // Stops, waiting for the source to be released (a camera can only
        // be opened once) when asked.
        void stop(bool wait = false);

        const CaptureSource& source() const { return _source; }

        // The last picture, when a new one came since the previous call.
        bool take(std::vector<uint8_t>& rgba);

        // Why there is no picture, if so.
        std::string error() const;

        // Shared with the thread of the capture.
        struct State;

    private:

        CaptureSource _source;
        std::shared_ptr<State> _state;
    };


    struct ScreenRecordSettings
    {
        CaptureSource source;
        // Frames per second, as a fraction (30000 / 1001 for 29.97).
        int frameRateNum = 30;
        int frameRateDen = 1;
        bool cursor = true;
        bool systemAudio = true;    // What the computer plays
    };


    // Records a screen or a window to a Matroska file (readable even when
    // the recording stops unexpectedly): Windows Graphics Capture through
    // the gfxcapture filter of FFmpeg, the pictures staying on the GPU up
    // to a hardware encoder (NVENC, AMF, Quick Sync; Media Foundation
    // otherwise), and the sound of the computer (WASAPI loopback) in AAC.
    //
    // Windows only for now: start() fails elsewhere.
    class ScreenRecorder
    {
    public:
        ScreenRecorder() = default;
        ~ScreenRecorder();

        ScreenRecorder(const ScreenRecorder&) = delete;
        ScreenRecorder& operator=(const ScreenRecorder&) = delete;

        // Opens the capture and the encoders, then records on threads of
        // its own until stop(). False with error set when it cannot start.
        bool start(const std::filesystem::path& path, const ScreenRecordSettings& settings, std::string& error);

        // Finishes the file. Returns false when the recording failed on
        // the way (see error()).
        bool stop();

        bool recording() const { return _recording.load(); }

        // Seconds recorded, from the pictures written.
        double seconds() const { return _seconds.load(); }

        // The last picture recorded, a few times a second, as the preview
        // (SourcePreview::Width x Height RGBA), when a new one came.
        bool takePreview(std::vector<uint8_t>& rgba);

        // Peak of the computer sound since the previous call.
        float takeAudioPeak() { return _audioPeak.exchange(0.f); }

        std::string error() const;
        const std::string& encoder() const { return _encoderName; }

    private:
        struct Video;
        struct Audio;

        void videoLoop();
        void audioLoop();
        void fail(const std::string& message);

        // Writes a packet of a stream, both threads sharing the file.
        int write(void* packet);
        void close();

        AVFormatContext* _format = nullptr;
        Video* _video = nullptr;
        Audio* _audio = nullptr;

        std::thread _videoThread;
        std::thread _audioThread;
        std::mutex _writeMutex;

        std::atomic<bool> _recording = false;
        std::atomic<bool> _stop = false;
        std::atomic<bool> _videoStarted = false;
        std::atomic<double> _seconds = 0.0;
        std::atomic<float> _audioPeak = 0.f;
        int _frameRateNum = 30;
        int _frameRateDen = 1;

        // Preview of the recording (video thread).
        std::mutex _previewMutex;
        std::vector<uint8_t> _preview;
        bool _previewFresh = false;
        uint64_t _previewTicks = 0;
        void* _previewSws = nullptr;
        void updatePreview(const void* picture);

        mutable std::mutex _errorMutex;
        std::string _error;
        std::string _encoderName;
    };
}
