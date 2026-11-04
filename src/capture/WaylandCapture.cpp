#include <capture/WaylandCapture.h>

#ifdef __linux__

#include <capture/LinuxCapture.h>

#include <core/Json.h>
#include <core/Paths.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <fcntl.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>


namespace capture::portal
{

namespace {

constexpr const char* PortalName = "org.freedesktop.portal.Desktop";
constexpr const char* PortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* ScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr const char* Prefix = "portal:";

// Kept after the last user lets a capture go, for the next one to take it.
constexpr auto Linger = std::chrono::seconds(5);

using Clock = std::chrono::steady_clock;


// ---------------------------------------------------------------------------
// Restore tokens, by key
// ---------------------------------------------------------------------------

std::mutex tokensMutex;

std::filesystem::path tokensPath()
{
    return core::configDirectory() / "screencast.json";
}


core::Json loadTokens()
{
    std::ifstream file(tokensPath(), std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();

    try {
        core::Json json = core::Json::parse(text.str());
        return json.isObject() ? json : core::Json::object();
    }
    catch (const std::exception&) {
        return core::Json::object();
    }
}


std::string token(const std::string& key)
{
    std::scoped_lock lock(tokensMutex);
    const core::Json tokens = loadTokens();
    return tokens[key].toString();
}


void saveToken(const std::string& key, const std::string& value)
{
    std::scoped_lock lock(tokensMutex);
    core::Json tokens = loadTokens();
    tokens[key] = value;

    std::error_code ec;
    std::filesystem::create_directories(tokensPath().parent_path(), ec);
    std::ofstream file(tokensPath(), std::ios::binary | std::ios::trunc);
    file << tokens.dump();
}


// ---------------------------------------------------------------------------
// D-Bus
// ---------------------------------------------------------------------------

std::string newToken()
{
    static std::atomic<uint32_t> counter = 0;
    std::random_device random;
    return "quickedit_" + std::to_string(random() & 0xffff) + "_" + std::to_string(++counter);
}


std::string message(GError* error)
{
    std::string text = error ? error->message : "unknown error";

    if (error) {
        g_error_free(error);
    }

    return text;
}


// The answer of a portal request: its Response signal.
struct Response
{
    bool received = false;
    uint32_t code = 2;
    GVariant* results = nullptr;
};


void onResponse(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters, gpointer data)
{
    Response& response = *static_cast<Response*>(data);
    g_variant_get(parameters, "(u@a{sv})", &response.code, &response.results);
    response.received = true;
}


// A portal session, opened on the thread that chooses the source: its
// requests answer on a main context of its own.
class Session
{
public:
    Session()
    {
        GError* error = nullptr;
        _bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);

        if (!_bus) {
            _error = "No session bus: " + message(error);
            return;
        }

        // The requests are answered at a path made of the sender's name.
        std::string sender = g_dbus_connection_get_unique_name(_bus) + 1;
        std::replace(sender.begin(), sender.end(), '.', '_');
        _sender = sender;

        _context = g_main_context_new();
    }

    ~Session()
    {
        close();

        if (_context) g_main_context_unref(_context);
        if (_bus) g_object_unref(_bus);
    }

    const std::string& error() const { return _error; }

    // Calls a method answered by a request, then waits for its response;
    // results then owned by the caller.
    bool request(const char* method, GVariant* parameters, const std::string& handleToken, const std::atomic<bool>* cancel, GVariant*& results)
    {
        const std::string path = std::string(PortalPath) + "/request/" + _sender + "/" + handleToken;
        Response response;

        // Subscribed before the call: no answer is missed.
        g_main_context_push_thread_default(_context);
        const guint subscription = g_dbus_connection_signal_subscribe(_bus, PortalName, "org.freedesktop.portal.Request", "Response",
            path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, onResponse, &response, nullptr);

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(_bus, PortalName, PortalPath, ScreenCast, method, parameters,
            G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);

        if (!reply) {
            _error = std::string(method) + " failed: " + message(error);
        }
        else {
            g_variant_unref(reply);

            while (!response.received && !(cancel && *cancel)) {
                while (g_main_context_iteration(_context, FALSE)) {}

                if (!response.received) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }

            if (!response.received) {
                _error = "Cancelled.";
            }
            else if (response.code != 0) {
                _error = response.code == 1 ? "The choice of the source was cancelled." : std::string(method) + " was refused by the portal.";
            }
        }

        g_dbus_connection_signal_unsubscribe(_bus, subscription);
        while (g_main_context_iteration(_context, FALSE)) {}
        g_main_context_pop_thread_default(_context);

        if (response.received && response.code == 0) {
            results = response.results;
            return true;
        }

        if (response.results) {
            g_variant_unref(response.results);
        }

        return false;
    }

    // Creates the session, has the source chosen (or restored) and started:
    // its PipeWire node, and the remote to reach it.
    bool start(uint32_t types, bool cursor, const std::string& restoreToken, const std::atomic<bool>* cancel,
        uint32_t& node, int& fd, std::string& newRestoreToken)
    {
        if (!_bus) {
            return false;
        }

        GVariant* results = nullptr;
        const std::string sessionToken = newToken();
        std::string handleToken = newToken();

        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(handleToken.c_str()));
        g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(sessionToken.c_str()));

        if (!request("CreateSession", g_variant_new("(a{sv})", &options), handleToken, cancel, results)) {
            return false;
        }

        const char* handle = nullptr;
        g_variant_lookup(results, "session_handle", "&s", &handle);
        _session = handle ? handle : "";
        g_variant_unref(results);

        if (_session.empty()) {
            _error = "The portal opened no session.";
            return false;
        }

        // Cursor: drawn in the pictures or not, when the portal can.
        const uint32_t cursorModes = property("AvailableCursorModes");
        const uint32_t cursorMode = cursor ? 2u : 1u;

        handleToken = newToken();
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(handleToken.c_str()));
        g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(types));
        g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(FALSE));

        if (cursorModes & cursorMode) {
            g_variant_builder_add(&options, "{sv}", "cursor_mode", g_variant_new_uint32(cursorMode));
        }

        // Remembered until revoked (version 4 of the portal).
        if (property("version") >= 4) {
            g_variant_builder_add(&options, "{sv}", "persist_mode", g_variant_new_uint32(2));

            if (!restoreToken.empty()) {
                g_variant_builder_add(&options, "{sv}", "restore_token", g_variant_new_string(restoreToken.c_str()));
            }
        }

        if (!request("SelectSources", g_variant_new("(oa{sv})", _session.c_str(), &options), handleToken, cancel, results)) {
            return false;
        }

        g_variant_unref(results);

        handleToken = newToken();
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(handleToken.c_str()));

        if (!request("Start", g_variant_new("(osa{sv})", _session.c_str(), "", &options), handleToken, cancel, results)) {
            return false;
        }

        const char* restore = nullptr;

        if (g_variant_lookup(results, "restore_token", "&s", &restore) && restore) {
            newRestoreToken = restore;
        }

        GVariant* streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
        bool found = false;

        if (streams && g_variant_n_children(streams) > 0) {
            GVariant* properties = nullptr;
            g_variant_get_child(streams, 0, "(u@a{sv})", &node, &properties);
            g_variant_unref(properties);
            found = true;
        }

        if (streams) g_variant_unref(streams);
        g_variant_unref(results);

        if (!found) {
            _error = "No source was shared.";
            return false;
        }

        // The PipeWire remote that gives the stream.
        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
        GUnixFDList* fds = nullptr;
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(_bus, PortalName, PortalPath, ScreenCast, "OpenPipeWireRemote",
            g_variant_new("(oa{sv})", _session.c_str(), &empty), G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, &error);

        if (!reply) {
            _error = "No PipeWire remote: " + message(error);
            return false;
        }

        gint32 index = 0;
        g_variant_get(reply, "(h)", &index);
        g_variant_unref(reply);
        fd = fds ? g_unix_fd_list_get(fds, index, &error) : -1;

        if (fds) g_object_unref(fds);

        if (fd < 0) {
            _error = "No PipeWire remote: " + message(error);
            return false;
        }

        return true;
    }

    void close()
    {
        if (_bus && !_session.empty()) {
            GVariant* reply = g_dbus_connection_call_sync(_bus, PortalName, _session.c_str(), "org.freedesktop.portal.Session", "Close",
                nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

            if (reply) {
                g_variant_unref(reply);
            }
        }

        _session.clear();
    }

private:
    uint32_t property(const char* name)
    {
        GVariant* reply = g_dbus_connection_call_sync(_bus, PortalName, PortalPath, "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", ScreenCast, name), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);

        if (!reply) {
            return 0;
        }

        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        const uint32_t result = g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(value) : 0;
        g_variant_unref(value);
        g_variant_unref(reply);

        return result;
    }

    GDBusConnection* _bus = nullptr;
    GMainContext* _context = nullptr;
    std::string _sender;
    std::string _session;
    std::string _error;
};


// ---------------------------------------------------------------------------
// PipeWire stream
// ---------------------------------------------------------------------------

AVPixelFormat pixelFormat(uint32_t format)
{
    // The alpha of the screens means nothing: ignored.
    switch (format) {
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGR0;
        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGB0;
        case SPA_VIDEO_FORMAT_xRGB:
        case SPA_VIDEO_FORMAT_ARGB: return AV_PIX_FMT_0RGB;
        case SPA_VIDEO_FORMAT_xBGR:
        case SPA_VIDEO_FORMAT_ABGR: return AV_PIX_FMT_0BGR;
        case SPA_VIDEO_FORMAT_RGB:  return AV_PIX_FMT_RGB24;
        case SPA_VIDEO_FORMAT_BGR:  return AV_PIX_FMT_BGR24;
        default:                    return AV_PIX_FMT_NONE;
    }
}


class Capture final : public Stream
{
public:
    enum class State
    {
        Opening,
        Ready,
        Failed
    };

    explicit Capture(bool cursor) : cursor(cursor) {}

    ~Capture() override
    {
        if (_loop) {
            pw_thread_loop_lock(_loop);

            if (_stream) {
                pw_stream_disconnect(_stream);
                pw_stream_destroy(_stream);
            }

            if (_core) {
                pw_core_disconnect(_core);
            }

            pw_thread_loop_unlock(_loop);
            pw_thread_loop_stop(_loop);

            if (_context) {
                pw_context_destroy(_context);
            }

            pw_thread_loop_destroy(_loop);
        }

        av_frame_free(&_latest);
        _session.reset();
    }

    const bool cursor;

    // Opening: on the thread of the first user, the others waiting.
    void open(uint32_t types, const std::string& key, const std::atomic<bool>* cancel)
    {
        _session = std::make_unique<Session>();
        uint32_t node = 0;
        int fd = -1;
        std::string restore;

        if (!_session->start(types, cursor, token(key), cancel, node, fd, restore)) {
            finish(State::Failed, _session->error().empty() ? "The portal could not start." : _session->error());
            return;
        }

        if (!restore.empty()) {
            saveToken(key, restore);
        }

        std::string error;

        if (!connect(fd, node, error)) {
            finish(State::Failed, error);
            return;
        }

        SDL_Log("Screen capture: PipeWire node %u of the portal", node);
        finish(State::Ready, {});
    }

    // Waits until opened (or cancelled): the error otherwise.
    bool waitOpened(const std::atomic<bool>* cancel, std::string& error)
    {
        std::unique_lock lock(_mutex);

        while (_state == State::Opening && !(cancel && *cancel)) {
            _changed.wait_for(lock, std::chrono::milliseconds(50));
        }

        if (_state != State::Ready) {
            error = _state == State::Opening ? "Cancelled." : _error;
            return false;
        }

        return true;
    }

    bool failed()
    {
        std::scoped_lock lock(_mutex);
        return _state == State::Failed;
    }

    bool next(AVFrame* frame, uint64_t& serial, int timeoutMs) override
    {
        std::unique_lock lock(_mutex);
        _changed.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] { return _serial != serial || _state == State::Failed; });

        if (_serial == serial || !_latest) {
            return false;
        }

        av_frame_unref(frame);

        if (av_frame_ref(frame, _latest) < 0) {
            return false;
        }

        serial = _serial;
        return true;
    }

    bool ended(std::string& error) const override
    {
        std::scoped_lock lock(_mutex);

        if (_state == State::Failed) {
            error = _error;
            return true;
        }

        return false;
    }

private:
    void finish(State state, const std::string& error)
    {
        {
            std::scoped_lock lock(_mutex);
            _state = state;
            _error = error;
        }

        if (state == State::Failed) {
            SDL_Log("Screen capture: %s", error.c_str());
        }

        _changed.notify_all();
    }

    bool connect(int fd, uint32_t node, std::string& error)
    {
        static std::once_flag initialized;
        std::call_once(initialized, [] { pw_init(nullptr, nullptr); });

        _loop = pw_thread_loop_new("QuickEdit capture", nullptr);
        _context = _loop ? pw_context_new(pw_thread_loop_get_loop(_loop), nullptr, 0) : nullptr;

        if (!_context || pw_thread_loop_start(_loop) < 0) {
            ::close(fd);
            error = "Could not start PipeWire.";
            return false;
        }

        pw_thread_loop_lock(_loop);

        // The remote takes the descriptor.
        _core = pw_context_connect_fd(_context, fcntl(fd, F_DUPFD_CLOEXEC, 5), nullptr, 0);
        ::close(fd);

        if (!_core) {
            pw_thread_loop_unlock(_loop);
            error = "Could not reach the PipeWire remote of the portal.";
            return false;
        }

        _stream = pw_stream_new(_core, "QuickEdit capture",
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));

        static const pw_stream_events events = [] {
            pw_stream_events e = {};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char* message) {
                static_cast<Capture*>(data)->stateChanged(state, message);
            };
            e.param_changed = [](void* data, uint32_t id, const spa_pod* param) {
                static_cast<Capture*>(data)->paramChanged(id, param);
            };
            e.process = [](void* data) { static_cast<Capture*>(data)->process(); };
            return e;
        }();

        pw_stream_add_listener(_stream, &_listener, &events, this);

        // Pictures in the memory (no DMA-BUF modifiers offered): the
        // compositors then give shared memory.
        uint8_t buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_rectangle size = SPA_RECTANGLE(1920, 1080);
        const spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        const spa_rectangle maxSize = SPA_RECTANGLE(16384, 16384);
        const spa_fraction rate = SPA_FRACTION(60, 1);
        const spa_fraction minRate = SPA_FRACTION(0, 1);
        const spa_fraction maxRate = SPA_FRACTION(1000, 1);

        const spa_pod* params[1];
        params[0] = (const spa_pod*)spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(9,
                SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA,
                SPA_VIDEO_FORMAT_xRGB, SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_RGB, SPA_VIDEO_FORMAT_BGR),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&size, &minSize, &maxSize),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &minRate, &maxRate));

        const int result = pw_stream_connect(_stream, PW_DIRECTION_INPUT, node,
            (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

        pw_thread_loop_unlock(_loop);

        if (result < 0) {
            error = std::string("Could not connect to the PipeWire stream: ") + spa_strerror(result);
            return false;
        }

        return true;
    }

    // PipeWire thread, from here on.

    void stateChanged(pw_stream_state state, const char* message)
    {
        if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && _streaming)) {
            finish(State::Failed, message && *message ? message : "The screen sharing stopped.");
        }

        if (state == PW_STREAM_STATE_STREAMING) {
            _streaming = true;
        }
    }

    void paramChanged(uint32_t id, const spa_pod* param)
    {
        if (!param || id != SPA_PARAM_Format) {
            return;
        }

        spa_video_info_raw info = {};

        if (spa_format_video_raw_parse(param, &info) < 0) {
            return;
        }

        _format = pixelFormat(info.format);
        _width = (int)info.size.width;
        _height = (int)info.size.height;
        _bytesPerPixel = (info.format == SPA_VIDEO_FORMAT_RGB || info.format == SPA_VIDEO_FORMAT_BGR) ? 3 : 4;
        SDL_Log("Screen capture: %u x %u %s", info.size.width, info.size.height, spa_debug_type_find_name(spa_type_video_format, info.format));

        uint8_t buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[2];

        params[0] = (const spa_pod*)spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));

        // Windows: the part of the buffer they fill.
        params[1] = (const spa_pod*)spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region)));

        pw_stream_update_params(_stream, params, 2);
    }

    void process()
    {
        // The newest buffer only.
        pw_buffer* buffer = pw_stream_dequeue_buffer(_stream);

        if (!buffer) {
            return;
        }

        while (pw_buffer* newer = pw_stream_dequeue_buffer(_stream)) {
            pw_stream_queue_buffer(_stream, buffer);
            buffer = newer;
        }

        copy(buffer->buffer);
        pw_stream_queue_buffer(_stream, buffer);
    }

    void copy(spa_buffer* buffer)
    {
        const spa_data& data = buffer->datas[0];

        if (_format == AV_PIX_FMT_NONE || !data.data || !data.chunk || data.chunk->size == 0 || (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
            return;
        }

        const int stride = data.chunk->stride;
        const uint8_t* pixels = static_cast<const uint8_t*>(data.data) + data.chunk->offset;
        int x = 0;
        int y = 0;
        int width = std::min(stride / _bytesPerPixel, _width);
        int height = stride > 0 ? std::min((int)(data.chunk->size / (uint32_t)stride), _height) : 0;

        if (spa_meta_region* crop = (spa_meta_region*)spa_buffer_find_meta_data(buffer, SPA_META_VideoCrop, sizeof(spa_meta_region));
            crop && spa_meta_region_is_valid(crop)) {
            x = std::clamp(crop->region.position.x, 0, width);
            y = std::clamp(crop->region.position.y, 0, height);
            width = std::min((int)crop->region.size.width, width - x);
            height = std::min((int)crop->region.size.height, height - y);
        }

        if (width < 2 || height < 2) {
            return;
        }

        AVFrame* frame = av_frame_alloc();
        frame->format = _format;
        frame->width = width;
        frame->height = height;

        if (av_frame_get_buffer(frame, 32) < 0) {
            av_frame_free(&frame);
            return;
        }

        const size_t row = (size_t)width * _bytesPerPixel;

        for (int i = 0; i < height; i++) {
            std::memcpy(frame->data[0] + (size_t)i * frame->linesize[0], pixels + (size_t)(y + i) * stride + (size_t)x * _bytesPerPixel, row);
        }

        frame->pts = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();

        {
            std::scoped_lock lock(_mutex);
            av_frame_free(&_latest);
            _latest = frame;
            _serial++;
        }

        _changed.notify_all();
    }

    std::unique_ptr<Session> _session;

    pw_thread_loop* _loop = nullptr;
    pw_context* _context = nullptr;
    pw_core* _core = nullptr;
    pw_stream* _stream = nullptr;
    spa_hook _listener = {};
    bool _streaming = false;

    AVPixelFormat _format = AV_PIX_FMT_NONE;
    int _bytesPerPixel = 4;
    int _width = 0;
    int _height = 0;

    mutable std::mutex _mutex;
    std::condition_variable _changed;
    State _state = State::Opening;
    std::string _error;
    AVFrame* _latest = nullptr;
    uint64_t _serial = 0;
};


// ---------------------------------------------------------------------------
// The captures open, by key
// ---------------------------------------------------------------------------

struct Registry
{
    struct Entry
    {
        std::shared_ptr<Capture> capture;
        Clock::time_point idleSince = Clock::time_point::max();
    };

    std::mutex mutex;
    std::map<std::string, Entry> entries;

    Registry()
    {
        // Lets go of the captures nobody used for a while (the system shows
        // the sharing until then).
        std::thread([this] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::vector<std::shared_ptr<Capture>> closed;

                {
                    std::scoped_lock lock(mutex);

                    for (auto it = entries.begin(); it != entries.end();) {
                        Entry& entry = it->second;
                        const bool used = entry.capture.use_count() > 1;

                        if (used) {
                            entry.idleSince = Clock::time_point::max();
                        }
                        else if (entry.idleSince == Clock::time_point::max()) {
                            entry.idleSince = Clock::now();
                        }

                        if (!used && (entry.capture->failed() || Clock::now() - entry.idleSince > Linger)) {
                            closed.push_back(std::move(entry.capture));
                            it = entries.erase(it);
                        }
                        else {
                            ++it;
                        }
                    }
                }

                // Closed out of the lock: the portal and PipeWire take a while.
                closed.clear();
            }
        }).detach();
    }
};


// Never destroyed: its thread runs until the end.
Registry& registry()
{
    static Registry* instance = new Registry();
    return *instance;
}


std::string keyOf(const CaptureSource& source)
{
    return source.device.substr(std::strlen(Prefix));
}

} // namespace


bool available()
{
    static const bool result = [] {
        if (!linux_capture::wayland()) {
            return false;
        }

        GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);

        if (!bus) {
            return false;
        }

        GVariant* reply = g_dbus_connection_call_sync(bus, PortalName, PortalPath, "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", ScreenCast, "AvailableSourceTypes"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);

        g_object_unref(bus);

        if (reply) {
            g_variant_unref(reply);
        }

        SDL_Log("Screen capture: ScreenCast portal %s", reply ? "available" : "missing");
        return reply != nullptr;
    }();

    return result;
}


bool isPortal(const CaptureSource& source)
{
    return source.kind != CaptureSource::Kind::Camera && source.device.rfind(Prefix, 0) == 0;
}


CaptureSource listed(CaptureSource::Kind kind)
{
    CaptureSource source;
    source.kind = kind;
    const bool monitor = kind == CaptureSource::Kind::Monitor;
    // Out of the range of the X11 monitors and windows.
    source.handle = monitor ? 0xffff0001ull : 0xffff0002ull;
    source.device = std::string(Prefix) + (monitor ? "screen" : "window");
    source.name = monitor ? "Screen (chosen in the system dialog)" : "Window (chosen in the system dialog)";
    return source;
}


CaptureSource own(const CaptureSource& source)
{
    if (!isPortal(source)) {
        return source;
    }

    std::random_device random;
    char key[32];
    std::snprintf(key, sizeof(key), "-%08x%08x", random(), random());

    CaptureSource result = source;
    result.device = std::string(Prefix) + (source.kind == CaptureSource::Kind::Monitor ? "screen" : "window") + key;
    return result;
}


std::shared_ptr<Stream> open(const CaptureSource& source, bool cursor, const std::atomic<bool>* cancel, std::string& error)
{
    if (!isPortal(source) || !available()) {
        error = "No screen sharing portal in this session.";
        return nullptr;
    }

    const std::string key = keyOf(source);
    Registry& captures = registry();
    std::shared_ptr<Capture> capture;
    bool opener = false;

    {
        std::scoped_lock lock(captures.mutex);
        auto it = captures.entries.find(key);

        if (it != captures.entries.end() && it->second.capture->failed()) {
            captures.entries.erase(it);
            it = captures.entries.end();
        }

        if (it == captures.entries.end()) {
            capture = std::make_shared<Capture>(cursor);
            captures.entries[key].capture = capture;
            opener = true;
        }
        else {
            capture = it->second.capture;
        }
    }

    if (opener) {
        // Portal source types: 1 the monitors, 2 the windows.
        capture->open(source.kind == CaptureSource::Kind::Monitor ? 1u : 2u, key, cancel);
    }

    if (!capture->waitOpened(cancel, error)) {
        return nullptr;
    }

    return capture;
}

} // namespace capture::portal

#endif
