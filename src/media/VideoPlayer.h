#pragma once

#include <media/VideoReader.h>

#include <core/Project.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>


namespace media
{
    // Decodes, on its own thread, the frame of a media to show in the
    // viewer. The main thread requests a media and a time on each UI frame
    // and takes the best frame decoded so far: it never waits for the
    // decoder.
    //
    // While playing, the thread decodes ahead of the requested time, so that
    // the next requests find their frame ready.
    class VideoPlayer
    {
    public:
        // hwDevice: FFmpeg Vulkan device, nullptr to decode on the CPU.
        explicit VideoPlayer(AVBufferRef* hwDevice);
        ~VideoPlayer();

        VideoPlayer(const VideoPlayer&) = delete;
        VideoPlayer& operator=(const VideoPlayer&) = delete;

        // Frame of the media at time (in the media) to show.
        void request(core::Id mediaId, const std::filesystem::path& path, core::Time time, bool playing);

        // Nothing to show.
        void clear();

        // Frame for the last request: the one covering its time once
        // decoded, the previous one of the same media until then, nullptr
        // when there is none.
        std::shared_ptr<VideoFrame> frame() const;

        // True when the media could not be opened or decoded.
        bool failed(core::Id mediaId) const;

        // Forgets the readers of the media, e.g. when it was removed.
        void forget(core::Id mediaId);

    private:
        struct Request
        {
            core::Id mediaId = core::InvalidId;
            std::filesystem::path path;
            core::Time time = 0;
            bool playing = false;
            uint64_t serial = 0;
        };

        void workerLoop();
        VideoReader* reader(const Request& request);

        AVBufferRef* _hwDevice = nullptr;

        mutable std::mutex _mutex;
        std::condition_variable _condition;
        Request _request;
        std::shared_ptr<VideoFrame> _current;
        core::Id _currentMedia = core::InvalidId;
        std::set<core::Id> _failed;
        std::set<core::Id> _forgotten;
        bool _stop = false;

        // Worker thread only.
        struct OpenReader
        {
            std::filesystem::path path;
            std::unique_ptr<VideoReader> reader;
        };

        std::map<core::Id, OpenReader> _readers;
        std::deque<core::Id> _readersUse;           // Most recent last
        std::deque<std::shared_ptr<VideoFrame>> _ahead;
        core::Id _aheadMedia = core::InvalidId;
        bool _aheadEnded = false;
        uint64_t _handledSerial = 0;

        std::thread _thread;
    };
}
