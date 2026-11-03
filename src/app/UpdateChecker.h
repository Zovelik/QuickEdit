#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>


// The latest release of QuickEdit on GitHub (its releases API, through
// FFmpeg's https), compared with this version, on a thread of its own.
// The repository is set at build time (QUICKEDIT_UPDATE_REPO); a private
// one answers 404 to anonymous requests: the check then fails.
class UpdateChecker
{
public:
    ~UpdateChecker();

    // owner/name of the repository; empty when the build has none.
    static const char* repository();

    void start();

    bool running() const { return _running.load(); }
    bool done() const { return _done.load(); }

    // The result taken: done no more.
    void acknowledge() { _done = false; }

    // Once done: the version of the latest release ("" when the check
    // failed, see error()), its page, and whether it is newer than this one.
    std::string latestVersion() const;
    std::string releaseUrl() const;
    bool newer() const;
    std::string error() const;

    // Version strings ("1.2.3", "v1.2.3-beta"): their numbers compared in
    // order. Negative, 0 or positive as a is older, the same, newer.
    static int compareVersions(const std::string& a, const std::string& b);

private:
    void run();

    std::thread _thread;
    std::atomic<bool> _running = false;
    std::atomic<bool> _done = false;

    mutable std::mutex _mutex;
    std::string _latest;
    std::string _url;
    std::string _error;
};
