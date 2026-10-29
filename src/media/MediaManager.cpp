#include <media/MediaManager.h>

#include <algorithm>


using namespace core;


namespace media
{

MediaManager::MediaManager(ThreadPool& pool)
    : _pool(pool)
{
}


MediaManager::~MediaManager()
{
    std::unique_lock lock(_mutex);

    for (auto& [id, entry] : _entries) {
        *entry.cancel = true;
    }

    _tasksDone.wait(lock, [this]() { return _pendingTasks == 0; });
}


void MediaManager::request(Id id, const std::filesystem::path& path, uint32_t sampleRate)
{
    auto cancel = std::make_shared<std::atomic<bool>>(false);

    {
        std::scoped_lock lock(_mutex);
        auto it = _entries.find(id);

        if (it != _entries.end()) {
            if (it->second.path == path && it->second.sampleRate == sampleRate) {
                return;
            }

            *it->second.cancel = true;
        }

        Entry& entry = _entries[id];
        entry = Entry();
        entry.path = path;
        entry.sampleRate = sampleRate;
        entry.cancel = cancel;

        _pendingTasks++;
    }

    _revision++;

    _pool.submit([this, id, path, sampleRate, cancel]() {
        analyze(id, path, sampleRate, cancel);

        std::scoped_lock lock(_mutex);
        _pendingTasks--;
        _tasksDone.notify_all();
    });
}


void MediaManager::sync(const Project& project)
{
    const uint32_t sampleRate = project.sequence().sampleRate;

    for (const MediaItem& item : project.media()) {
        request(item.id, item.path, sampleRate);
    }

    std::scoped_lock lock(_mutex);

    for (auto it = _entries.begin(); it != _entries.end();) {
        if (!project.findMedia(it->first)) {
            *it->second.cancel = true;
            it = _entries.erase(it);
            _revision++;
        }
        else {
            ++it;
        }
    }
}


void MediaManager::clear()
{
    std::scoped_lock lock(_mutex);

    for (auto& [id, entry] : _entries) {
        *entry.cancel = true;
    }

    _entries.clear();
    _revision++;
}


MediaState MediaManager::state(Id id) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _entries.find(id);

    return (it != _entries.end()) ? it->second.state : MediaState();
}


std::shared_ptr<const AudioBuffer> MediaManager::audio(Id id) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _entries.find(id);

    return (it != _entries.end()) ? it->second.audio : nullptr;
}


std::shared_ptr<const Waveform> MediaManager::waveform(Id id) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _entries.find(id);

    return (it != _entries.end()) ? it->second.waveform : nullptr;
}


bool MediaManager::applyProbeResults(Project& project) const
{
    std::scoped_lock lock(_mutex);
    bool changed = false;

    for (MediaItem& item : project.media()) {
        const auto it = _entries.find(item.id);

        if (it == _entries.end() || !it->second.state.probe || item.probed) {
            continue;
        }

        applyProbe(item, *it->second.state.probe);
        changed = true;
    }

    return changed;
}


void MediaManager::applyProbe(MediaItem& item, const ProbeResult& probe)
{
    item.probed = true;
    item.type = !probe.hasVideo ? MediaType::Audio : (probe.isImage ? MediaType::Image : MediaType::Video);

    // A still image has no length of its own: keep the default one.
    if (item.type != MediaType::Image && probe.duration > 0) {
        item.duration = probe.duration;
    }

    item.width = probe.width;
    item.height = probe.height;
    item.frameRate = probe.frameRate;
    item.hasAudio = probe.hasAudio;
    item.sampleRate = probe.sampleRate;
    item.channels = probe.channels;
}


template<typename F>
void MediaManager::update(Id id, const std::shared_ptr<std::atomic<bool>>& cancel, F&& change)
{
    {
        std::scoped_lock lock(_mutex);
        const auto it = _entries.find(id);

        if (it == _entries.end() || it->second.cancel != cancel) {
            return;
        }

        change(it->second);
    }

    _revision++;
}


void MediaManager::analyze(Id id, std::filesystem::path path, uint32_t sampleRate, std::shared_ptr<std::atomic<bool>> cancel)
{
    if (*cancel) {
        return;
    }

    try {
        const ProbeResult result = probe(path);

        update(id, cancel, [&](Entry& entry) {
            entry.state.probe = result;
            entry.state.status = (result.hasAudio && !result.isImage) ? MediaStatus::Decoding : MediaStatus::Ready;
        });

        if (!result.hasAudio || result.isImage) {
            return;
        }

        // Progress is published every percent, not on every packet.
        int lastPercent = -1;

        auto buffer = std::make_shared<AudioBuffer>(decodeAudio(path, sampleRate, *cancel, [&](float progress) {
            const int percent = (int)(progress * 100.f);

            if (percent != lastPercent) {
                lastPercent = percent;
                update(id, cancel, [&](Entry& entry) { entry.state.progress = progress; });
            }
        }));

        if (*cancel) {
            return;
        }

        auto waveform = std::make_shared<Waveform>(Waveform::build(*buffer));

        update(id, cancel, [&](Entry& entry) {
            entry.audio = std::move(buffer);
            entry.waveform = std::move(waveform);
            entry.state.status = MediaStatus::Ready;
            entry.state.progress = 1.f;
        });
    }
    catch (const std::exception& e) {
        update(id, cancel, [&](Entry& entry) {
            entry.state.status = MediaStatus::Error;
            entry.state.error = e.what();
        });
    }
}

}
