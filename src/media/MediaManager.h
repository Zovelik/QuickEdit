#pragma once

#include <media/AudioBuffer.h>
#include <media/MediaFile.h>

#include <core/Project.h>
#include <core/ThreadPool.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>


namespace media
{
    enum class MediaStatus
    {
        Probing,
        Decoding,       // Probed, audio being decoded
        Ready,
        Error
    };


    struct MediaState
    {
        MediaStatus status = MediaStatus::Probing;
        float progress = 0.f;           // Of the decoding, in [0, 1]
        std::string error;
        std::optional<ProbeResult> probe;
    };


    // Analyzes the media of a project in the background: probing, audio
    // decoding and waveforms, one task per media on the thread pool.
    //
    // The project itself is only modified by the main thread, through
    // applyProbeResults(). The decoded audio and waveforms are immutable and
    // shared, so the audio thread and the UI read them without locking.
    class MediaManager
    {
    public:
        // The pool must outlive the manager.
        explicit MediaManager(core::ThreadPool& pool);

        // Cancels the running tasks and waits for them.
        ~MediaManager();

        // Starts analyzing a media, unless it is already analyzed for the
        // same file and sample rate.
        void request(core::Id id, const std::filesystem::path& path, uint32_t sampleRate);

        // Requests every media of the project, and forgets the media it no
        // longer holds.
        void sync(const core::Project& project);

        void clear();

        // Incremented whenever a state, an audio buffer or a waveform changes.
        uint64_t revision() const { return _revision.load(); }

        MediaState state(core::Id id) const;
        // The sound of a media: all its audio streams together (stream -1),
        // or one of them.
        std::shared_ptr<const AudioBuffer> audio(core::Id id, int stream = -1) const;
        std::shared_ptr<const Waveform> waveform(core::Id id, int stream = -1) const;

        // Copies the probed properties into the media items of the project.
        // Returns true when anything changed. Called after each analysis and
        // after undo / redo, which restore the snapshots taken before it.
        bool applyProbeResults(core::Project& project) const;

        // Copies the properties of a file into its media item.
        static void applyProbe(core::MediaItem& item, const ProbeResult& probe);

    private:
        struct Entry
        {
            std::filesystem::path path;
            uint32_t sampleRate = 0;
            MediaState state;
            std::shared_ptr<const AudioBuffer> audio;           // All the streams
            std::shared_ptr<const Waveform> waveform;

            // Each stream, when the file has several.
            std::vector<std::shared_ptr<const AudioBuffer>> streams;
            std::vector<std::shared_ptr<const Waveform>> streamWaveforms;

            // Mixes of some of the streams, made when asked, by stream
            // number (-2 - the mask of the streams left out).
            mutable std::map<int, std::shared_ptr<const AudioBuffer>> mixes;
            mutable std::map<int, std::shared_ptr<const Waveform>> mixWaveforms;
            std::shared_ptr<std::atomic<bool>> cancel;
        };

        void analyze(core::Id id, std::filesystem::path path, uint32_t sampleRate, std::shared_ptr<std::atomic<bool>> cancel);

        // Updates the entry of the task, unless the task was superseded.
        template<typename F>
        void update(core::Id id, const std::shared_ptr<std::atomic<bool>>& cancel, F&& change);

        core::ThreadPool& _pool;

        mutable std::mutex _mutex;
        std::map<core::Id, Entry> _entries;
        std::atomic<uint64_t> _revision = 1;

        // Tasks submitted and not finished, see ~MediaManager().
        int _pendingTasks = 0;
        std::condition_variable _tasksDone;
    };
}
