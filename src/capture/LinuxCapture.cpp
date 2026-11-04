#include <capture/LinuxCapture.h>

#ifdef __linux__

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>


namespace capture::linux_capture
{

namespace {

// Windows can go while they are listed: their errors are ignored.
int ignoreErrors(Display*, XErrorEvent*)
{
    return 0;
}


// One connection for the listing, its errors ignored meanwhile.
class Connection
{
public:
    Connection()
    {
        const std::string name = display();

        if (!name.empty()) {
            _display = XOpenDisplay(name.c_str());
        }

        _previous = XSetErrorHandler(ignoreErrors);
    }

    ~Connection()
    {
        if (_display) {
            XSync(_display, False);
            XCloseDisplay(_display);
        }

        XSetErrorHandler(_previous);
    }

    Display* get() const { return _display; }

private:
    Display* _display = nullptr;
    XErrorHandler _previous = nullptr;
};


// A property of a window, as items of a format (32-bit items come as longs).
std::vector<unsigned char> property(Display* display, Window window, const char* name, Atom type, int& format, unsigned long& count)
{
    const Atom atom = XInternAtom(display, name, True);
    std::vector<unsigned char> data;
    count = 0;
    format = 0;

    if (atom == None) {
        return data;
    }

    Atom actualType = None;
    unsigned long remaining = 0;
    unsigned char* value = nullptr;

    if (XGetWindowProperty(display, window, atom, 0, 1 << 16, False, type, &actualType, &format, &count, &remaining, &value) == Success && value) {
        const size_t itemSize = (format == 32) ? sizeof(long) : (size_t)format / 8;
        data.assign(value, value + count * itemSize);
        XFree(value);
    }

    return data;
}


std::string windowTitle(Display* display, Window window)
{
    int format = 0;
    unsigned long count = 0;
    const Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    std::vector<unsigned char> name = property(display, window, "_NET_WM_NAME", utf8, format, count);

    if (!name.empty()) {
        return std::string(name.begin(), name.end());
    }

    char* text = nullptr;

    if (XFetchName(display, window, &text) && text) {
        std::string title = text;
        XFree(text);
        return title;
    }

    return {};
}


int xioctl(int fd, unsigned long request, void* argument)
{
    int result = 0;

    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);

    return result;
}

} // namespace


std::string display()
{
    const char* name = std::getenv("DISPLAY");
    return (name && *name) ? name : std::string();
}


bool wayland()
{
    const char* session = std::getenv("XDG_SESSION_TYPE");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");

    return (session && std::strcmp(session, "wayland") == 0) || (waylandDisplay && *waylandDisplay);
}


std::vector<CaptureSource> monitors()
{
    std::vector<CaptureSource> sources;
    Connection connection;
    Display* x = connection.get();

    if (!x) {
        return sources;
    }

    const Window root = DefaultRootWindow(x);
    int count = 0;
    XRRMonitorInfo* list = XRRGetMonitors(x, root, True, &count);

    for (int i = 0; i < count; i++) {
        const XRRMonitorInfo& monitor = list[i];
        CaptureSource source;
        source.kind = CaptureSource::Kind::Monitor;
        source.handle = (uint64_t)i + 1;
        source.x = monitor.x;
        source.y = monitor.y;
        source.width = monitor.width;
        source.height = monitor.height;

        char* atomName = monitor.name ? XGetAtomName(x, monitor.name) : nullptr;
        std::string name = "Screen " + std::to_string(i + 1);

        if (atomName) {
            name += std::string(" ") + atomName;
            XFree(atomName);
        }

        name += "  (" + std::to_string(monitor.width) + " x " + std::to_string(monitor.height) + (monitor.primary ? ", main)" : ")");
        source.name = name;
        sources.push_back(std::move(source));
    }

    if (list) {
        XRRFreeMonitors(list);
    }

    // No XRandR monitors: the whole display.
    if (sources.empty()) {
        CaptureSource source;
        source.kind = CaptureSource::Kind::Monitor;
        source.handle = 1;
        source.width = DisplayWidth(x, DefaultScreen(x));
        source.height = DisplayHeight(x, DefaultScreen(x));
        source.name = "Screen  (" + std::to_string(source.width) + " x " + std::to_string(source.height) + ")";
        sources.push_back(std::move(source));
    }

    return sources;
}


std::vector<CaptureSource> windows()
{
    std::vector<CaptureSource> sources;
    Connection connection;
    Display* x = connection.get();

    if (!x) {
        return sources;
    }

    // The windows of the applications, as the window manager lists them.
    int format = 0;
    unsigned long count = 0;
    const std::vector<unsigned char> list = property(x, DefaultRootWindow(x), "_NET_CLIENT_LIST", XA_WINDOW, format, count);
    const long* ids = reinterpret_cast<const long*>(list.data());
    const long ownProcess = (long)getpid();

    for (unsigned long i = 0; i < count && format == 32; i++) {
        const Window window = (Window)ids[i];
        XWindowAttributes attributes = {};

        // Shown, and not one of QuickEdit.
        if (!XGetWindowAttributes(x, window, &attributes) || attributes.map_state != IsViewable || attributes.width < 16 || attributes.height < 16) {
            continue;
        }

        int pidFormat = 0;
        unsigned long pidCount = 0;
        const std::vector<unsigned char> pid = property(x, window, "_NET_WM_PID", XA_CARDINAL, pidFormat, pidCount);

        if (pidCount == 1 && pidFormat == 32 && *reinterpret_cast<const long*>(pid.data()) == ownProcess) {
            continue;
        }

        const std::string title = windowTitle(x, window);

        if (title.empty()) {
            continue;
        }

        std::string application;
        XClassHint hint = {};

        if (XGetClassHint(x, window, &hint)) {
            if (hint.res_name) application = hint.res_name;
            if (hint.res_name) XFree(hint.res_name);
            if (hint.res_class) XFree(hint.res_class);
        }

        CaptureSource source;
        source.kind = CaptureSource::Kind::Window;
        source.handle = (uint64_t)window;
        source.width = attributes.width;
        source.height = attributes.height;
        source.name = application.empty() ? title : title + "  (" + application + ")";
        sources.push_back(std::move(source));
    }

    return sources;
}


bool windowSize(uint64_t window, int& width, int& height)
{
    Connection connection;
    XWindowAttributes attributes = {};

    if (!connection.get() || !XGetWindowAttributes(connection.get(), (Window)window, &attributes)) {
        return false;
    }

    width = attributes.width;
    height = attributes.height;
    return width > 0 && height > 0;
}


std::vector<CaptureSource> cameras()
{
    std::vector<CaptureSource> sources;

    for (int i = 0; i < 64; i++) {
        const std::string device = "/dev/video" + std::to_string(i);
        const int fd = open(device.c_str(), O_RDWR | O_NONBLOCK);

        if (fd < 0) {
            continue;
        }

        v4l2_capability capability = {};

        // A node that gives pictures (a camera has another one for its
        // metadata).
        if (xioctl(fd, VIDIOC_QUERYCAP, &capability) == 0) {
            const uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;

            if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
                CaptureSource source;
                source.kind = CaptureSource::Kind::Camera;
                source.handle = (uint64_t)sources.size() + 1;
                source.device = device;
                source.name = reinterpret_cast<const char*>(capability.card);
                sources.push_back(std::move(source));
            }
        }

        close(fd);
    }

    return sources;
}


std::vector<CameraFormat> cameraFormats(const std::string& device)
{
    // Read once per camera: opening it takes a moment.
    static std::mutex mutex;
    static std::map<std::string, std::vector<CameraFormat>> cache;

    {
        std::scoped_lock lock(mutex);

        if (const auto it = cache.find(device); it != cache.end()) {
            return it->second;
        }
    }

    std::vector<CameraFormat> formats;
    const int fd = open(device.c_str(), O_RDWR | O_NONBLOCK);

    if (fd < 0) {
        return formats;
    }

    v4l2_fmtdesc description = {};
    description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (description.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &description) == 0; description.index++) {
        CameraFormat base;

        switch (description.pixelformat) {
            case V4L2_PIX_FMT_MJPEG: base.codec = "mjpeg"; break;
            case V4L2_PIX_FMT_H264:  base.codec = "h264"; break;
            case V4L2_PIX_FMT_YUYV:  base.pixelFormat = "yuyv422"; break;
            case V4L2_PIX_FMT_UYVY:  base.pixelFormat = "uyvy422"; break;
            case V4L2_PIX_FMT_NV12:  base.pixelFormat = "nv12"; break;
            case V4L2_PIX_FMT_YUV420: base.pixelFormat = "yuv420p"; break;
            default: continue;
        }

        v4l2_frmsizeenum size = {};
        size.pixel_format = description.pixelformat;

        for (size.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0 && size.type == V4L2_FRMSIZE_TYPE_DISCRETE; size.index++) {
            CameraFormat format = base;
            format.width = (int)size.discrete.width;
            format.height = (int)size.discrete.height;

            v4l2_frmivalenum interval = {};
            interval.pixel_format = description.pixelformat;
            interval.width = size.discrete.width;
            interval.height = size.discrete.height;

            for (interval.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0; interval.index++) {
                if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE && interval.discrete.numerator > 0) {
                    format.frameRates.push_back((double)interval.discrete.denominator / (double)interval.discrete.numerator);
                }
            }

            std::sort(format.frameRates.begin(), format.frameRates.end(), std::greater<double>());
            formats.push_back(std::move(format));
        }
    }

    close(fd);

    // The largest first, compressed before raw at a size.
    std::stable_sort(formats.begin(), formats.end(), [](const CameraFormat& a, const CameraFormat& b) {
        return a.width * a.height > b.width * b.height;
    });

    SDL_Log("Camera %s: %d formats", device.c_str(), (int)formats.size());

    std::scoped_lock lock(mutex);
    cache[device] = formats;
    return formats;
}

}

#endif
