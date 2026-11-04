#pragma once

// Screens and windows of a Wayland session, through the ScreenCast desktop
// portal (xdg-desktop-portal, over D-Bus) and the PipeWire stream it opens.
//
// The source is chosen in the dialog of the system, not listed: a portal
// source is a CaptureSource whose device is "portal:<key>". The portal
// gives a restore token with each choice, kept by key in the settings
// (screencast.json), so that the same source opens again without the
// dialog: one key for the Record Screen window ("screen", "window"), one of
// its own for each source of a scene.
//
// One capture is shared by all who open the same key (the preview, then the
// recording), and kept a few seconds after the last one lets it go: the
// recording takes over the stream of the preview without another dialog.

#ifdef __linux__

#include <capture/ScreenRecorder.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>


struct AVFrame;


namespace capture::portal
{
    // True when the session bus has the ScreenCast portal.
    bool available();

    bool isPortal(const CaptureSource& source);

    // The sources listed: a screen or a window chosen in the dialog.
    CaptureSource listed(CaptureSource::Kind kind);

    // A copy of a listed source remembering its own choice.
    CaptureSource own(const CaptureSource& source);


    class Stream
    {
    public:
        virtual ~Stream() = default;

        // A picture newer than serial (then updated), waiting for it up to
        // timeoutMs: referenced into frame, BGR0, RGB0, BGR24 or RGB24, its
        // pts in microseconds. False when none came.
        virtual bool next(AVFrame* frame, uint64_t& serial, int timeoutMs) = 0;

        // The stream ended (the sharing stopped, the window closed...).
        virtual bool ended(std::string& error) const = 0;
    };

    // The capture of a portal source, shown the dialog when the source has
    // no valid restore token. Waits for the choice, until cancel is set.
    // The first to open a source chooses whether the cursor is drawn.
    std::shared_ptr<Stream> open(const CaptureSource& source, bool cursor, const std::atomic<bool>* cancel, std::string& error);
}

#endif
