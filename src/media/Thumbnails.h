#pragma once

#include <core/ThreadPool.h>
#include <core/Time.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <vector>


namespace media
{
    // Small pictures of a video at regular times (a single one for an
    // image), decoded in the background into an atlas of RGBA tiles,
    // Columns per row, in time order.
    struct ThumbnailStrip
    {
        static constexpr int TileWidth = 128;
        static constexpr int TileHeight = 72;
        static constexpr int Columns = 8;

        core::Time interval = core::TicksPerSecond;
        int count = 1;

        int atlasWidth() const { return Columns * TileWidth; }
        int atlasHeight() const { return ((count + Columns - 1) / Columns) * TileHeight; }

        // Tiles decoded so far, from the first; the pixels with them.
        std::mutex mutex;
        std::vector<uint8_t> rgba;
        int ready = 0;
        bool dirty = false;             // New tiles since the last take

        std::atomic<bool> cancel = false;
    };


    // The strips of the files, made on demand on the thread pool, kept for
    // the session (by path: another project finds them again).
    class ThumbnailCache
    {
    public:
        explicit ThumbnailCache(core::ThreadPool& pool) : _pool(pool) {}
        ~ThumbnailCache();

        std::shared_ptr<ThumbnailStrip> get(const std::filesystem::path& path, core::Time duration, bool image);

    private:
        core::ThreadPool& _pool;
        std::mutex _mutex;
        std::map<std::filesystem::path, std::shared_ptr<ThumbnailStrip>> _strips;
    };
}
