#pragma once

// Linux side of the capture (see ScreenRecorder.h): the sources of an X11
// display (its monitors with XRandR, the windows of the window manager),
// the V4L2 cameras and their formats. Pictures and sound are then read by
// FFmpeg (x11grab, v4l2, pulse).

#ifdef __linux__

#include <capture/ScreenRecorder.h>

#include <string>
#include <vector>


namespace capture::linux_capture
{
    // The X11 display to capture (DISPLAY), empty when there is none.
    std::string display();

    // True in a Wayland session: x11grab only sees the X11 (XWayland)
    // windows there; the screens need the desktop portal (to come).
    bool wayland();

    std::vector<CaptureSource> monitors();
    std::vector<CaptureSource> windows();
    std::vector<CaptureSource> cameras();

    // The formats of a V4L2 camera, the largest first.
    std::vector<CameraFormat> cameraFormats(const std::string& device);

    // Current size of a window (0 when it is gone).
    bool windowSize(uint64_t window, int& width, int& height);
}

#endif
