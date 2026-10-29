#pragma once

#include <core/Project.h>
#include <media/AudioBuffer.h>

#include <imgui.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>


// Timeline of a sequence: the time ruler, the track headers and the clips.
// Draws inside the current ImGui window and edits the project directly;
// each finished edit is reported so that the caller records it in the undo
// history.
//
// Mouse:
//   Click on the ruler / drag           move the playhead
//   Click on a clip (Ctrl to add)       select it, with its linked clips
//   Drag a clip                         move it, also to another track
//   Drag the edge of a clip             trim it
//   Wheel                               scroll in time
//   Ctrl + Wheel                        zoom around the mouse
//   Shift + Wheel                       scroll the tracks
//
// Media dropped from the media bin ("QE_MEDIA" payload holding a core::Id)
// are placed where they are dropped.
class TimelineView
{
public:
    // ImGui drag and drop payload type of the media items.
    static constexpr const char* MediaPayload = "QE_MEDIA";

    // A sequence of the project dropped to be nested ("QE_SEQUENCE", its id).
    static constexpr const char* SequencePayload = "QE_SEQUENCE";

    struct Result
    {
        // An edit was finished during this frame.
        bool edited = false;
        std::string label;

        // The playhead was moved by the user.
        bool seeked = false;

        // Effects button of an audio track clicked.
        core::Id effectsOfTrack = core::InvalidId;

        // A nested sequence double clicked: to show.
        core::Id openSequence = core::InvalidId;

        // Files of the file manager dropped on a track (see setFileDrop()),
        // to place one after the other from dropStart.
        core::Id dropTrack = core::InvalidId;
        core::Time dropStart = 0;
    };

    // Files dragged from the file manager, previewed where they would land.
    // Set before each draw() while the drag lasts.
    struct FileDrop
    {
        bool active = false;
        bool dropped = false;

        // Their lengths, empty while not all known.
        std::vector<core::Time> durations;
    };

    void setFileDrop(FileDrop drop) { _fileDrop = std::move(drop); }

    // Recordings being made, drawn on their tracks (with the waveform of
    // the sound). Added before each draw() while they last.
    struct LiveRecording
    {
        core::Id trackId = core::InvalidId;
        core::Time start = 0;
        core::Time length = 0;
        std::shared_ptr<const media::Waveform> waveform;
    };

    void addLiveRecording(LiveRecording recording) { _liveRecordings.push_back(std::move(recording)); }

    // Selection helpers: the linked clips of a selection, and whether a clip
    // is selected.
    static void addWithLinks(const core::Project& project, core::Id clipId, std::vector<core::Id>& selection);

    Result draw(core::Project& project, core::Time& playhead, std::vector<core::Id>& selection);

    // Waveform drawn in the audio clips of a media, nullptr when not ready.
    std::function<std::shared_ptr<const media::Waveform>(core::Id mediaId)> waveformOf;

    // Pictures drawn in the video clips of a media: an atlas of tiles
    // (Columns per row, TileWidth x TileHeight of atlasWidth x atlasHeight),
    // one every interval of the media, the first ready ones usable.
    struct Thumbnails
    {
        ImTextureRef texture;
        int count = 0;
        int ready = 0;
        core::Time interval = 0;
        bool image = false;
        int columns = 1;
        float tileU = 1.f;              // Size of a tile in the atlas, in texture coordinates
        float tileV = 1.f;
    };

    std::function<Thumbnails(core::Id mediaId)> thumbnailsOf;

    // The shaders of the user, offered with the built-in transitions.
    const std::vector<core::ShaderDef>* userShaders = nullptr;

    // Empty range between two clips of a track (or before the first one),
    // selected by a click: Delete closes it.
    struct Gap
    {
        core::Id trackId = core::InvalidId;
        core::Time start = 0;
        core::Time end = 0;
    };

    bool hasGap() const { return _gap.trackId != core::InvalidId; }
    const Gap& gap() const { return _gap; }
    void clearGap() { _gap = Gap(); }

    bool snapping() const { return _snapping; }
    void setSnapping(bool snapping) { _snapping = snapping; }

    // Horizontal zoom, in pixels per second.
    float zoom() const { return _pixelsPerSecond; }
    void setZoom(float pixelsPerSecond);

    // Zooms to show the whole sequence on the next frame.
    void requestZoomToFit() { _zoomToFitRequested = true; }

    // Keeps the playhead visible during playback.
    void followPlayhead(core::Time playhead);

private:
    enum class DragMode
    {
        None,
        Scrub,
        Move,
        TrimStart,
        TrimEnd,
        FadeIn,         // Audio fade handles
        FadeOut
    };

    // A clip taken by a move or trim, as it was when the drag started.
    struct DragItem
    {
        core::Clip original;
        core::TrackType trackType;
        int originalTrackIndex;
    };

    // A row of the timeline: one track, top to bottom.
    struct Row
    {
        core::TrackType type;
        int trackIndex;
        float top;
        float height;
    };

    void layoutRows(const core::Sequence& sequence, float top);
    const Row* rowAt(float y) const;

    float timeToX(core::Time t) const;
    core::Time xToTime(float x) const;

    // One frame of the sequence, in ticks.
    core::Time frameDuration(const core::Sequence& sequence) const;
    core::Time roundToFrame(core::Time t, const core::Sequence& sequence) const;

    void drawRuler(ImDrawList* drawList, const core::Sequence& sequence, const ImVec2& min, const ImVec2& max) const;
    bool drawTrackHeaders(ImDrawList* drawList, core::Sequence& sequence, std::string& label, core::Id& effectsOfTrack);
    void drawClips(ImDrawList* drawList, const core::Project& project, const std::vector<core::Id>& selection) const;

    // Waveform of an audio clip inside the rectangle [min, max] of the clip.
    void drawWaveform(ImDrawList* drawList, const core::Clip& clip, const media::Waveform& waveform, const ImVec2& min, const ImVec2& max) const;

    void beginDrag(core::Project& project, DragMode mode, const core::Clip& grabbed, const std::vector<core::Id>& selection);

    // Places the dragged clips for the given mouse position, keeping the
    // last valid placement when the new one would overlap other clips.
    void updateDrag(core::Project& project, float mouseX, float mouseY);

    // True when the drag changed anything.
    bool endDrag(core::Project& project);

    // Snaps the time t to the playhead or to a clip edge close to it on
    // screen. Returns t when nothing is close enough.
    core::Time snapTime(const core::Project& project, core::Time t, bool excludeDragged) const;

    static std::vector<core::Track>& tracksOfType(core::Sequence& sequence, core::TrackType type);
    static int groupOf(core::TrackType type);

    // Headers of the groups of tracks (sound of the videos, audio): fold,
    // mute the group.
    bool drawGroupHeaders(ImDrawList* drawList, core::Sequence& sequence, std::string& label);
    static const std::vector<core::Track>& tracksOfType(const core::Sequence& sequence, core::TrackType type);

    static bool isSelected(const std::vector<core::Id>& selection, core::Id clipId);

    // View
    float _pixelsPerSecond = 50.f;
    double _scrollSeconds = 0.0;
    float _scrollY = 0.f;
    bool _snapping = true;
    Gap _gap;
    FileDrop _fileDrop;
    std::vector<LiveRecording> _liveRecordings;
    bool _zoomToFitRequested = false;

    // Layout of the current frame, in screen coordinates.
    float _headerWidth = 0.f;
    float _timelineLeft = 0.f;
    float _timelineRight = 0.f;
    float _tracksTop = 0.f;
    float _tracksBottom = 0.f;
    std::vector<Row> _rows;

    struct GroupHeader
    {
        core::TrackType type;
        float top;
        float height;
    };

    std::vector<GroupHeader> _groupHeaders;
    core::Time _playhead = 0;

    // Drag in progress
    DragMode _dragMode = DragMode::None;
    std::vector<DragItem> _dragItems;
    core::Id _grabbedClip = core::InvalidId;
    core::TrackType _grabbedType = core::TrackType::Video;
    int _grabbedTrackIndex = 0;
    float _dragStartX = 0.f;
    float _dragStartY = 0.f;
    core::Time _lastValidDelta = 0;
    int _lastValidTrackDelta = 0;

    // Time of the edge the drag snapped to, -1 when none.
    core::Time _snapLine = -1;
};
