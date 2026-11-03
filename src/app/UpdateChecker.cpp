#include <app/UpdateChecker.h>

#include <core/Guard.h>
#include <core/Json.h>

#include <config.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <vector>


UpdateChecker::~UpdateChecker()
{
    if (_thread.joinable()) {
        _thread.join();
    }
}


const char* UpdateChecker::repository()
{
#ifdef UPDATE_REPO_STR
    return UPDATE_REPO_STR;
#else
    return "";
#endif
}


void UpdateChecker::start()
{
    if (_running || !*repository()) {
        return;
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    _running = true;
    _done = false;

    _thread = std::thread([this]() {
        if (!core::guarded("Update check", [this]() { run(); })) {
            std::scoped_lock lock(_mutex);
            _error = "The check failed.";
        }

        _done = true;
        _running = false;
    });
}


void UpdateChecker::run()
{
    const std::string url = std::string("https://api.github.com/repos/") + repository() + "/releases/latest";

    // GitHub wants a user agent.
    AVDictionary* options = nullptr;
    av_dict_set(&options, "user_agent", "QuickEdit/" APP_VERSION_STR, 0);
    av_dict_set(&options, "headers", "Accept: application/vnd.github+json\r\n", 0);
    av_dict_set(&options, "timeout", "10000000", 0);    // Microseconds

    AVIOContext* input = nullptr;
    const int result = avio_open2(&input, url.c_str(), AVIO_FLAG_READ, nullptr, &options);
    av_dict_free(&options);

    std::string latest;
    std::string page;
    std::string error;

    if (result < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(result, text, sizeof(text));

        // 404: no release, or a private repository (not found for those not
        // signed in).
        error = (result == AVERROR_HTTP_NOT_FOUND)
            ? "No release found (the repository may be private)."
            : std::string("Could not reach GitHub: ") + text;
    }
    else {
        std::string body;
        std::vector<unsigned char> buffer(1 << 16);

        while (body.size() < (1 << 20)) {
            const int count = avio_read(input, buffer.data(), (int)buffer.size());

            if (count <= 0) {
                break;
            }

            body.append((const char*)buffer.data(), (size_t)count);
        }

        avio_closep(&input);

        try {
            const core::Json release = core::Json::parse(body);
            latest = release["tag_name"].toString();
            page = release["html_url"].toString();

            if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) {
                latest.erase(0, 1);
            }

            if (latest.empty()) {
                error = "No release found.";
            }
        }
        catch (const std::exception& e) {
            error = std::string("Unexpected answer from GitHub: ") + e.what();
        }
    }

    if (!error.empty()) {
        SDL_Log("Update check: %s", error.c_str());
    }
    else {
        SDL_Log("Update check: latest release %s (this one %s)", latest.c_str(), APP_VERSION_STR);
    }

    std::scoped_lock lock(_mutex);
    _latest = latest;
    _url = page;
    _error = error;
}


std::string UpdateChecker::latestVersion() const
{
    std::scoped_lock lock(_mutex);
    return _latest;
}


std::string UpdateChecker::releaseUrl() const
{
    std::scoped_lock lock(_mutex);
    return _url;
}


bool UpdateChecker::newer() const
{
    std::scoped_lock lock(_mutex);
    return !_latest.empty() && compareVersions(_latest, APP_VERSION_STR) > 0;
}


std::string UpdateChecker::error() const
{
    std::scoped_lock lock(_mutex);
    return _error;
}


int UpdateChecker::compareVersions(const std::string& a, const std::string& b)
{
    // The numbers of a version, until something else than digits and dots.
    auto numbers = [](const std::string& version) {
        std::vector<long> parts;
        size_t i = (!version.empty() && (version[0] == 'v' || version[0] == 'V')) ? 1 : 0;

        while (i < version.size() && std::isdigit((unsigned char)version[i])) {
            long value = 0;

            while (i < version.size() && std::isdigit((unsigned char)version[i])) {
                value = value * 10 + (version[i++] - '0');
            }

            parts.push_back(value);

            if (i < version.size() && version[i] == '.') {
                i++;
            }
            else {
                break;
            }
        }

        return parts;
    };

    const std::vector<long> left = numbers(a);
    const std::vector<long> right = numbers(b);

    for (size_t i = 0; i < std::max(left.size(), right.size()); i++) {
        const long x = (i < left.size()) ? left[i] : 0;
        const long y = (i < right.size()) ? right[i] : 0;

        if (x != y) {
            return x < y ? -1 : 1;
        }
    }

    return 0;
}
