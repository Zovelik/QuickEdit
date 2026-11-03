#include <media/VideoPlayer.h>

#include <core/Guard.h>

extern "C" {
#include <libavutil/buffer.h>
}

#include <algorithm>


using namespace core;


namespace media
{

namespace {

// Frames decoded ahead of the playhead while playing.
constexpr size_t MaxFramesAhead = 8;

// Files kept open, the most recently used ones.
constexpr size_t MaxOpenReaders = 4;

} // namespace


VideoPlayer::VideoPlayer(AVBufferRef* hwDevice)
    : _hwDevice(hwDevice ? av_buffer_ref(hwDevice) : nullptr)
{
    _thread = std::thread([this]() {
        // An error not handled in the loop: the request given up (its media
        // failed, nothing decoded ahead), then the loop goes on.
        while (!guarded("Video decoding", [this]() { workerLoop(); })) {
            _ahead.clear();
            _aheadEnded = true;

            std::scoped_lock lock(_mutex);
            _failed.insert(_request.mediaId);
            _handledSerial = _request.serial;
        }
    });
}


VideoPlayer::~VideoPlayer()
{
    {
        std::scoped_lock lock(_mutex);
        _stop = true;
    }

    _condition.notify_all();
    _thread.join();

    // Frames and readers hold references on the device: released first.
    _current.reset();
    _ahead.clear();
    _readers.clear();
    av_buffer_unref(&_hwDevice);
}


void VideoPlayer::request(Id mediaId, const std::filesystem::path& path, Time time, bool playing)
{
    {
        std::scoped_lock lock(_mutex);

        if (_request.mediaId == mediaId && _request.time == time && _request.playing == playing && _request.path == path) {
            return;
        }

        // Another file under the same id: its frames are not this one's.
        if (_request.mediaId == mediaId && _request.path != path) {
            _current.reset();
            _currentMedia = InvalidId;
        }

        _request.mediaId = mediaId;
        _request.path = path;
        _request.time = time;
        _request.playing = playing;
        _request.serial++;
    }

    _condition.notify_all();
}


void VideoPlayer::clear()
{
    request(InvalidId, {}, 0, false);
}


std::shared_ptr<VideoFrame> VideoPlayer::frame() const
{
    std::scoped_lock lock(_mutex);

    // A frame of another media is never shown in place of the requested one.
    return (_currentMedia == _request.mediaId) ? _current : nullptr;
}


bool VideoPlayer::failed(Id mediaId) const
{
    std::scoped_lock lock(_mutex);
    return _failed.count(mediaId) != 0;
}


void VideoPlayer::forget(Id mediaId)
{
    {
        std::scoped_lock lock(_mutex);
        _forgotten.insert(mediaId);
        _failed.erase(mediaId);
        _request.serial++;
    }

    _condition.notify_all();
}


VideoReader* VideoPlayer::reader(const Request& request)
{
    auto it = _readers.find(request.mediaId);

    // Another file under the same id (the media was relinked, or another
    // project): reopened.
    if (it != _readers.end() && it->second.path != request.path) {
        _readers.erase(it);
        it = _readers.end();

        if (_aheadMedia == request.mediaId) {
            _ahead.clear();
            _aheadMedia = InvalidId;
        }
    }

    if (it == _readers.end()) {
        try {
            it = _readers.emplace(request.mediaId, OpenReader{ request.path, std::make_unique<VideoReader>(request.path, _hwDevice) }).first;
        }
        catch (const std::exception&) {
            std::scoped_lock lock(_mutex);
            _failed.insert(request.mediaId);
            return nullptr;
        }

        // Close the least recently used files.
        while (_readers.size() > MaxOpenReaders && !_readersUse.empty()) {
            const Id oldest = _readersUse.front();
            _readersUse.pop_front();

            if (oldest != request.mediaId) {
                _readers.erase(oldest);
            }
        }
    }

    std::erase(_readersUse, request.mediaId);
    _readersUse.push_back(request.mediaId);

    return it->second.reader.get();
}


void VideoPlayer::workerLoop()
{
    while (true) {
        Request request;
        std::set<Id> forgotten;

        {
            std::unique_lock lock(_mutex);

            _condition.wait(lock, [&]() {
                const bool prefetch = _request.playing && !_aheadEnded && _ahead.size() < MaxFramesAhead &&
                    _aheadMedia == _request.mediaId && _request.mediaId != InvalidId;

                return _stop || _request.serial != _handledSerial || prefetch;
            });

            if (_stop) {
                return;
            }

            request = _request;
            forgotten.swap(_forgotten);
        }

        for (Id id : forgotten) {
            _readers.erase(id);
            std::erase(_readersUse, id);

            if (_aheadMedia == id) {
                _ahead.clear();
                _aheadMedia = InvalidId;
            }
        }

        const bool newRequest = request.serial != _handledSerial;
        _handledSerial = request.serial;

        if (request.mediaId == InvalidId) {
            std::scoped_lock lock(_mutex);
            _current.reset();
            _currentMedia = InvalidId;
            continue;
        }

        VideoReader* videoReader = reader(request);

        if (!videoReader) {
            continue;
        }

        if (_aheadMedia != request.mediaId) {
            _ahead.clear();
            _aheadMedia = request.mediaId;
            _aheadEnded = false;
        }

        // Frames already shown are dropped.
        while (_ahead.size() > 1 && _ahead.front()->end() <= request.time) {
            _ahead.pop_front();
        }

        std::shared_ptr<VideoFrame> frame;

        if (newRequest) {
            // In the frames decoded ahead, or from the reader (seeking if
            // needed).
            for (const std::shared_ptr<VideoFrame>& ahead : _ahead) {
                if (ahead->covers(request.time)) {
                    frame = ahead;
                    break;
                }
            }

            if (!frame) {
                _ahead.clear();
                _aheadEnded = false;

                try {
                    frame = videoReader->frameAt(request.time);
                }
                catch (const std::exception&) {
                    std::scoped_lock lock(_mutex);
                    _failed.insert(request.mediaId);
                    continue;
                }

                if (frame) {
                    _ahead.push_back(frame);
                }
            }
        }
        else if (!videoReader->stillImage()) {
            // Prefetch while playing.
            std::shared_ptr<VideoFrame> next;

            try {
                next = videoReader->nextFrame();
            }
            catch (const std::exception&) {
                std::scoped_lock lock(_mutex);
                _failed.insert(request.mediaId);
                _aheadEnded = true;
                continue;
            }

            if (!next) {
                _aheadEnded = true;
                continue;
            }

            _ahead.push_back(next);

            if (next->covers(request.time)) {
                frame = next;
            }
        }
        else {
            _aheadEnded = true;
            continue;
        }

        if (frame) {
            std::scoped_lock lock(_mutex);
            _current = frame;
            _currentMedia = request.mediaId;
        }
    }
}

}
