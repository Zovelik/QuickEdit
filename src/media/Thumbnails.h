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
    // Columns per row, in time order. Of a level of detail: tiles of
    // 128 x 72 pixels for level 0, twice as large at each level above.
    struct ThumbnailStrip
    {
        static constexpr int Columns = 8;
        static constexpr int Levels = 3;

        int tileWidth = 128;
        int tileHeight = 72;
        core::Time interval = core::TicksPerSecond;
        int count = 1;

        int atlasWidth() const { return Columns * tileWidth; }
        int atlasHeight() const { return ((count + Columns - 1) / Columns) * tileHeight; }

        // Tiles decoded so far, from the first; the pixels with them.
        std::mutex mutex;
        std::vector<uint8_t> rgba;
        int ready = 0;

        std::atomic<bool> cancel = false;
    };


    // The strips of the files, made on demand on the thread pool, kept for
    // the session (by path: another project finds them again).
    class ThumbnailCache
    {
    public:
        explicit ThumbnailCache(core::ThreadPool& pool) : _pool(pool) {}
        ~ThumbnailCache();

        // The strip of a file at a level of detail (0 to Levels - 1),
        // started when first asked.
        std::shared_ptr<ThumbnailStrip> get(const std::filesystem::path& path, core::Time duration, bool image, int level = 0);

    private:
        core::ThreadPool& _pool;
        std::mutex _mutex;
        std::map<std::pair<std::filesystem::path, int>, std::shared_ptr<ThumbnailStrip>> _strips;
    };
}
