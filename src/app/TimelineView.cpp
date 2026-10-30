#include <app/TimelineView.h>
#include <app/I18n.h>

#include <fonts/IconsMaterialDesign.h>

#include <core/Layers.h>

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>


using namespace core;


namespace {

// Colors of the clips, by track type.
constexpr ImU32 VideoClipColor         = IM_COL32( 52,  98, 160, 255);
constexpr ImU32 VideoClipSelectedColor = IM_COL32( 84, 142, 214, 255);
constexpr ImU32 AudioClipColor         = IM_COL32( 46, 128,  92, 255);
constexpr ImU32 AudioClipSelectedColor = IM_COL32( 70, 176, 128, 255);
constexpr ImU32 TextClipColor          = IM_COL32(124,  78, 168, 255);
constexpr ImU32 TextClipSelectedColor  = IM_COL32(166, 116, 214, 255);
constexpr ImU32 FrozenClipColor        = IM_COL32( 60, 120, 150, 255);
constexpr ImU32 GeneratorClipColor     = IM_COL32(168, 104,  40, 255);
constexpr ImU32 SequenceClipColor      = IM_COL32( 40, 130, 130, 255);
constexpr ImU32 SubtitleClipColor      = IM_COL32( 92, 100, 118, 255);
constexpr ImU32 SubtitleClipSelectedColor = IM_COL32(136, 146, 170, 255);
constexpr ImU32 SequenceClipSelectedColor = IM_COL32( 70, 180, 180, 255);
constexpr ImU32 GeneratorClipSelectedColor = IM_COL32(214, 146,  70, 255);
constexpr ImU32 PlayheadColor          = IM_COL32(255, 122,   0, 255);
constexpr ImU32 SnapLineColor          = IM_COL32(255, 230, 120, 200);

// Steps of the ruler graduations, in seconds (frame steps come first, see
// drawRuler()).
constexpr double RulerSteps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600 };

constexpr float MinZoom = 0.05f;      // pixels per second
constexpr float MaxZoom = 4000.f;


// Square icon button drawn without an ImGui item: the timeline handles the
// mouse itself. Returns true when clicked.
bool headerButton(ImDrawList* drawList, const ImVec2& min, float size, const char* icon, bool active, bool canvasHovered)
{
    const ImVec2 max(min.x + size, min.y + size);
    const bool hovered = canvasHovered && ImGui::IsMouseHoveringRect(min, max);

    if (active || hovered) {
        const ImU32 color = active ? ImGui::GetColorU32(ImGuiCol_ButtonActive) : ImGui::GetColorU32(ImGuiCol_ButtonHovered, 0.5f);
        drawList->AddRectFilled(min, max, color, ImGui::GetStyle().FrameRounding);
    }

    const ImVec2 textSize = ImGui::CalcTextSize(icon);
    drawList->AddText(
        ImVec2(min.x + 0.5f * (size - textSize.x), min.y + 0.5f * (size - textSize.y)),
        ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled),
        icon);

    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// Height of a sample value in a waveform, in [-1, 1]. On a decibel scale
// down to WaveformFloorDb, as the quiet parts of a voice (breaths, noise
// between words) matter as much as the loud ones when cutting.
constexpr float WaveformFloorDb = -48.f;

float displayLevel(float value)
{
    const float magnitude = std::min(std::abs(value), 1.f);

    if (magnitude <= 0.f) {
        return 0.f;
    }

    const float db = 20.f * std::log10(magnitude);
    const float level = std::max(0.f, 1.f - db / WaveformFloorDb);

    return std::copysign(level, value);
}

} // namespace


std::vector<Track>& TimelineView::tracksOfType(Sequence& sequence, TrackType type)
{
    return sequence.tracks(type);
}


const std::vector<Track>& TimelineView::tracksOfType(const Sequence& sequence, TrackType type)
{
    return sequence.tracks(type);
}


int TimelineView::groupOf(TrackType type)
{
    // A video track and its sound track move together.
    return (type == TrackType::Audio) ? 1 : (type == TrackType::Subtitle ? 2 : 0);
}


bool TimelineView::isSelected(const std::vector<Id>& selection, Id clipId)
{
    return std::find(selection.begin(), selection.end(), clipId) != selection.end();
}


void TimelineView::addWithLinks(const Project& project, Id clipId, std::vector<Id>& selection)
{
    Id linkGroup = InvalidId;

    for (const auto* tracks : { &project.sequence().videoTracks, &project.sequence().soundTracks, &project.sequence().audioTracks, &project.sequence().subtitleTracks }) {
        for (const Track& track : *tracks) {
            for (const Clip& clip : track.clips) {
                if (clip.id == clipId) linkGroup = clip.linkGroup;
            }
        }
    }

    for (const auto* tracks : { &project.sequence().videoTracks, &project.sequence().soundTracks, &project.sequence().audioTracks, &project.sequence().subtitleTracks }) {
        for (const Track& track : *tracks) {
            for (const Clip& clip : track.clips) {
                const bool linked = clip.id == clipId || (linkGroup != InvalidId && clip.linkGroup == linkGroup);

                if (linked && !isSelected(selection, clip.id)) {
                    selection.push_back(clip.id);
                }
            }
        }
    }
}


void TimelineView::setZoom(float pixelsPerSecond)
{
    _pixelsPerSecond = std::clamp(pixelsPerSecond, MinZoom, MaxZoom);
}


void TimelineView::followPlayhead(Time playhead)
{
    const float x = timeToX(playhead);
    const float width = _timelineRight - _timelineLeft;

    if (width <= 0.f) {
        return;
    }

    if (x > _timelineRight - 0.05f * width || x < _timelineLeft) {
        _scrollSeconds = std::max(0.0, timeToSeconds(playhead) - 0.1 * width / _pixelsPerSecond);
    }
}


float TimelineView::timeToX(Time t) const
{
    return _timelineLeft + (float)((timeToSeconds(t) - _scrollSeconds) * _pixelsPerSecond);
}


Time TimelineView::xToTime(float x) const
{
    return secondsToTime(_scrollSeconds + (double)(x - _timelineLeft) / _pixelsPerSecond);
}


Time TimelineView::frameDuration(const Sequence& sequence) const
{
    return std::max<Time>(1, frameToTime(1, sequence.frameRate));
}


Time TimelineView::roundToFrame(Time t, const Sequence& sequence) const
{
    const Time frame = frameDuration(sequence);
    const Time half = frame / 2;

    return snapToFrame(t >= 0 ? t + half : t - half, sequence.frameRate);
}


void TimelineView::layoutRows(const Sequence& sequence, float top)
{
    const float unit = ImGui::GetFontSize();
    const float videoHeight = 2.8f * unit;
    const float audioHeight = 3.4f * unit;
    const float gap = 0.4f * unit;

    const float headerHeight = 1.5f * unit;

    _rows.clear();
    _groupHeaders.clear();
    float y = top - _scrollY;

    // Top video track first: they are composited bottom to top.
    for (int i = (int)sequence.videoTracks.size() - 1; i >= 0; i--) {
        _rows.push_back({ TrackType::Video, i, y, videoHeight });
        y += videoHeight;
    }

    y += gap;

    // The sound of the videos, foldable.
    _groupHeaders.push_back({ TrackType::Sound, y, headerHeight });
    y += headerHeight;

    if (!sequence.soundTracksFolded) {
        for (int i = 0; i < (int)sequence.soundTracks.size(); i++) {
            _rows.push_back({ TrackType::Sound, i, y, audioHeight });
            y += audioHeight;
        }
    }

    y += gap;

    // Voice, music...
    _groupHeaders.push_back({ TrackType::Audio, y, headerHeight });
    y += headerHeight;

    for (int i = 0; i < (int)sequence.audioTracks.size(); i++) {
        _rows.push_back({ TrackType::Audio, i, y, audioHeight });
        y += audioHeight;
    }

    // Subtitles, when there are some.
    if (!sequence.subtitleTracks.empty()) {
        y += gap;
        _groupHeaders.push_back({ TrackType::Subtitle, y, headerHeight });
        y += headerHeight;

        for (int i = 0; i < (int)sequence.subtitleTracks.size(); i++) {
            _rows.push_back({ TrackType::Subtitle, i, y, 2.f * unit });
            y += 2.f * unit;
        }
    }
}


const TimelineView::Row* TimelineView::rowAt(float y) const
{
    for (const Row& row : _rows) {
        if (y >= row.top && y < row.top + row.height) {
            return &row;
        }
    }

    return nullptr;
}


TimelineView::Result TimelineView::draw(Project& project, Time& playhead, std::vector<Id>& selection)
{
    Result result;
    Sequence& sequence = project.sequence();
    ImGuiIO& io = ImGui::GetIO();

    const float unit = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    if (avail.x < 4.f * unit || avail.y < 3.f * unit) {
        return result;
    }

    const float rulerHeight = 1.8f * unit;

    _headerWidth   = 9.f * unit;
    _timelineLeft  = origin.x + _headerWidth;
    _timelineRight = origin.x + avail.x;
    _tracksTop     = origin.y + rulerHeight;
    _tracksBottom  = origin.y + avail.y;
    _playhead      = playhead;

    if (_zoomToFitRequested) {
        _zoomToFitRequested = false;
        const double seconds = std::max(10.0, timeToSeconds(sequence.duration()) * 1.05);
        setZoom((float)((_timelineRight - _timelineLeft) / seconds));
        _scrollSeconds = 0.0;
    }

    // The tracks may be taller than the panel: clamp the vertical scrolling.
    layoutRows(sequence, _tracksTop);

    if (!_rows.empty()) {
        const float contentHeight = (_rows.back().top + _rows.back().height) - (_rows.front().top);
        const float maxScrollY = std::max(0.f, contentHeight - (_tracksBottom - _tracksTop));
        const float clamped = std::clamp(_scrollY, 0.f, maxScrollY);

        if (clamped != _scrollY) {
            _scrollY = clamped;
            layoutRows(sequence, _tracksTop);
        }
    }

    // A single item covers the whole timeline: the mouse is handled below.
    ImGui::InvisibleButton("##timeline", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // ------------------------------------------------------------------------
    // Wheel: scroll and zoom
    // ------------------------------------------------------------------------

    if (hovered && _dragMode == DragMode::None) {
        if (io.KeyCtrl && io.MouseWheel != 0.f) {
            // Zoom around the time under the mouse.
            const double mouseSeconds = timeToSeconds(xToTime(std::max(mouse.x, _timelineLeft)));
            setZoom(_pixelsPerSecond * std::pow(1.25f, io.MouseWheel));
            _scrollSeconds = mouseSeconds - (double)(std::max(mouse.x, _timelineLeft) - _timelineLeft) / _pixelsPerSecond;
        }
        else if (io.KeyShift && io.MouseWheel != 0.f) {
            _scrollY -= io.MouseWheel * 2.f * unit;
        }
        else if (io.MouseWheel != 0.f || io.MouseWheelH != 0.f) {
            const float wheel = (io.MouseWheel != 0.f) ? io.MouseWheel : -io.MouseWheelH;
            _scrollSeconds -= (double)(wheel * 6.f * unit) / _pixelsPerSecond;
        }

        _scrollSeconds = std::max(0.0, _scrollSeconds);
    }

    // Middle button: drag the view.
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        _scrollSeconds = std::max(0.0, _scrollSeconds - (double)io.MouseDelta.x / _pixelsPerSecond);
        _scrollY -= io.MouseDelta.y;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    // ------------------------------------------------------------------------
    // Background, tracks and clips
    // ------------------------------------------------------------------------

    drawList->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), ImGui::GetColorU32(ImGuiCol_FrameBg, 0.6f));

    drawList->PushClipRect(ImVec2(origin.x, _tracksTop), ImVec2(_timelineRight, _tracksBottom), true);

    for (const Row& row : _rows) {
        const Track& track = tracksOfType(sequence, row.type)[row.trackIndex];
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_WindowBg, track.locked ? 0.5f : 1.f);

        drawList->AddRectFilled(
            ImVec2(_timelineLeft, row.top + 1.f),
            ImVec2(_timelineRight, row.top + row.height - 1.f),
            color);
    }

    drawList->PushClipRect(ImVec2(_timelineLeft, _tracksTop), ImVec2(_timelineRight, _tracksBottom), true);
    drawClips(drawList, project, selection);
    drawList->PopClipRect();

    if (drawTrackHeaders(drawList, sequence, result.label, result.effectsOfTrack)) {
        result.edited = true;
    }

    if (drawGroupHeaders(drawList, sequence, result.label)) {
        result.edited = true;
    }

    drawList->PopClipRect();

    // ------------------------------------------------------------------------
    // Mouse: selection, move, trim, scrub
    // ------------------------------------------------------------------------

    const float edgeWidth = 0.45f * unit;

    // Clip and edge under the mouse.
    const Row* hoveredRow = (mouse.y >= _tracksTop && mouse.y < _tracksBottom) ? rowAt(mouse.y) : nullptr;
    const Clip* hoveredClip = nullptr;
    DragMode hoveredMode = DragMode::None;

    if (hovered && hoveredRow && mouse.x >= _timelineLeft) {
        const Track& track = tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex];

        for (const Clip& clip : track.clips) {
            const float x0 = timeToX(clip.start);
            const float x1 = timeToX(clip.end());

            if (mouse.x >= x0 - edgeWidth * 0.5f && mouse.x < x1 + edgeWidth * 0.5f) {
                hoveredClip = &clip;

                // Edges are only grabbed on clips wide enough to keep a body.
                const bool wide = (x1 - x0) > 3.f * edgeWidth;

                // Audio fades: handles at the top corners, where the fades end.
                const bool top = hoveredRow->type != TrackType::Video && mouse.y < hoveredRow->top + 0.7f * unit;
                const float fadeInX = timeToX(clip.start + clip.fadeIn);
                const float fadeOutX = timeToX(clip.end() - clip.fadeOut);

                if (top && wide && std::abs(mouse.x - std::max(fadeInX, x0 + 0.5f * edgeWidth)) < edgeWidth)       hoveredMode = DragMode::FadeIn;
                else if (top && wide && std::abs(mouse.x - std::min(fadeOutX, x1 - 0.5f * edgeWidth)) < edgeWidth) hoveredMode = DragMode::FadeOut;
                else if (wide && mouse.x < x0 + edgeWidth)      hoveredMode = DragMode::TrimStart;
                else if (wide && mouse.x > x1 - edgeWidth) hoveredMode = DragMode::TrimEnd;
                else                                       hoveredMode = DragMode::Move;

                if (track.locked) {
                    hoveredMode = DragMode::None;
                }

                break;
            }
        }
    }

    // The other tools than Select do not trim, nor change the fades.
    if (_tool != Tool::Select && hoveredMode != DragMode::None) {
        hoveredMode = DragMode::Move;
    }

    if (_dragMode == DragMode::Slip || (_dragMode == DragMode::None && _tool == Tool::Slip && hoveredClip)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    // Blade: where the cut would be, across the track.
    if (_tool == Tool::Blade && _dragMode == DragMode::None && hoveredClip && hoveredRow) {
        const float x = timeToX(snapToFrame(snapTime(project, xToTime(mouse.x), false), sequence.frameRate));
        const float top = io.KeyShift ? _tracksTop : hoveredRow->top;
        const float bottom = io.KeyShift ? _tracksBottom : hoveredRow->top + hoveredRow->height;

        drawList->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(255, 80, 60, 230), 2.f);
    }

    if (_dragMode == DragMode::TrimStart || _dragMode == DragMode::TrimEnd ||
        (_dragMode == DragMode::None && (hoveredMode == DragMode::TrimStart || hoveredMode == DragMode::TrimEnd))) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if (_dragMode == DragMode::FadeIn || _dragMode == DragMode::FadeOut ||
        (_dragMode == DragMode::None && (hoveredMode == DragMode::FadeIn || hoveredMode == DragMode::FadeOut))) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // A click handled by a tool: no selection of a gap after it.
    bool toolClick = false;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && _dragMode == DragMode::None) {
        if (mouse.y < _tracksTop && mouse.x >= _timelineLeft) {
            _dragMode = DragMode::Scrub;
        }
        else if (_tool != Tool::Select && mouse.x >= _timelineLeft && mouse.y >= _tracksTop) {
            toolClick = true;

            if (toolPressed(project, hoveredClip, hoveredRow, mouse, selection)) {
                result.edited = true;
                result.label = "Split clips";
            }

            // The clips may have moved in memory.
            hoveredClip = nullptr;
        }
        else if (hoveredClip && mouse.x >= _timelineLeft) {
            if (io.KeyAlt) {
                // The clip alone, without its linked clips (the sound of a
                // video, to delete or move it apart).
                if (!io.KeyCtrl) {
                    selection.clear();
                }

                if (!isSelected(selection, hoveredClip->id)) {
                    selection.push_back(hoveredClip->id);
                }
            }
            else if (io.KeyCtrl) {
                if (isSelected(selection, hoveredClip->id)) {
                    std::erase(selection, hoveredClip->id);
                }
                else {
                    addWithLinks(project, hoveredClip->id, selection);
                }
            }
            else if (!isSelected(selection, hoveredClip->id)) {
                selection.clear();
                addWithLinks(project, hoveredClip->id, selection);
            }

            if (hoveredMode != DragMode::None && isSelected(selection, hoveredClip->id)) {
                // The pointer is invalidated by the drag, which moves clips
                // between tracks: copy it first.
                const Clip grabbed = *hoveredClip;
                beginDrag(project, hoveredMode, grabbed, selection);
            }
        }
        else if (mouse.x >= _timelineLeft) {
            // A rectangle may follow: the clips it touches selected.
            _dragMode = DragMode::Marquee;
            _marqueeTime = xToTime(mouse.x);
            _marqueeX = mouse.x;
            _marqueeY = mouse.y + _scrollY;
            _marqueeBase = io.KeyCtrl ? selection : std::vector<Id>();
        }

        if (!toolClick && !hoveredClip && mouse.x >= _timelineLeft && mouse.y >= _tracksTop && !io.KeyCtrl) {
            selection.clear();
            _gap = Gap();

            // Between two clips, or before the first one.
            if (hoveredRow) {
                const Track& track = tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex];
                Time freeStart, freeEnd;

                if (project.freeRangeAt(track, xToTime(mouse.x), InvalidId, freeStart, freeEnd) &&
                    freeEnd < std::numeric_limits<Time>::max() / 8 && freeEnd > freeStart) {
                    _gap = { track.id, freeStart, freeEnd };
                }
            }
        }
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredClip) {
        _gap = Gap();
    }

    // Double click on a nested sequence: shown.
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoveredClip && hoveredClip->kind == ClipKind::Sequence) {
        result.openSequence = hoveredClip->sequence;
    }

    // Right click on the selected gap: close it.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !hoveredClip && hasGap() && hoveredRow &&
        tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex].id == _gap.trackId) {
        ImGui::OpenPopup("##gapContext");
    }

    if (ImGui::BeginPopup("##gapContext")) {
        if (ImGui::MenuItem(tr(ICON_MD_DELETE_SWEEP " Close Gap"), "Del")) {
            if (project.closeGap(_gap.trackId, _gap.start, _gap.end)) {
                result.edited = true;
                result.label = "Close gap";
            }

            _gap = Gap();
        }

        ImGui::EndPopup();
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hoveredClip && _dragMode == DragMode::None) {
        if (!isSelected(selection, hoveredClip->id)) {
            selection.clear();
            addWithLinks(project, hoveredClip->id, selection);
        }

        // The track of the clip: its sound may become subtitles.
        const Track* track = project.findTrackOfClip(hoveredClip->id);
        _contextTrack = (track && (track->type == TrackType::Sound || track->type == TrackType::Audio)) ? track->id : InvalidId;

        ImGui::OpenPopup("##clipContext");
    }

    // Right click on a track header, or on the timeline away from the clips
    // (and from the selected gap): the tracks.
    const bool onGap = hasGap() && hoveredRow && tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex].id == _gap.trackId;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && _dragMode == DragMode::None &&
        (mouse.x < _timelineLeft || (!hoveredClip && !onGap)) && mouse.y >= _tracksTop) {
        _contextTrack = hoveredRow ? tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex].id : InvalidId;
        ImGui::OpenPopup("##trackContext");
    }

    if (ImGui::BeginPopup("##trackContext")) {
        const Track* track = project.findTrack(_contextTrack);

        if (track) {
            ImGui::TextDisabled(tr("Track %s"), track->name.c_str());

            if ((track->type == TrackType::Sound || track->type == TrackType::Audio) &&
                ImGui::MenuItem(tr(ICON_MD_RECORD_VOICE_OVER " Generate Subtitles from this Track..."))) {
                result.generateSubtitles = _contextTrack;
            }

            ImGui::Separator();
        }

        auto addItem = [&](const char* label, TrackType type) {
            if (ImGui::MenuItem(tr(label))) {
                result.addTrack = true;
                result.addTrackType = type;
            }
        };

        addItem(ICON_MD_LIBRARY_ADD " Add Video Track", TrackType::Video);
        addItem(ICON_MD_PLAYLIST_ADD " Add Audio Track", TrackType::Audio);
        addItem(ICON_MD_SUBTITLES " Add Subtitle Track", TrackType::Subtitle);

        if (track) {
            ImGui::Separator();

            // A video track goes with its sound track; one video track stays.
            const bool paired = track->type == TrackType::Video || track->type == TrackType::Sound;
            const char* label = (track->type == TrackType::Sound) ? tr(ICON_MD_DELETE " Delete Track (with its Video Track)")
                : (track->type == TrackType::Video) ? tr(ICON_MD_DELETE " Delete Track (with its Sound Track)")
                : tr(ICON_MD_DELETE " Delete Track");

            if (ImGui::MenuItem(label, nullptr, false, !paired || sequence.videoTracks.size() > 1)) {
                result.deleteTrack = _contextTrack;
            }
        }

        ImGui::EndPopup();
    }

    if (_dragMode == DragMode::FadeIn || _dragMode == DragMode::FadeOut) {
        if (Clip* clip = project.findClip(_grabbedClip)) {
            const Time t = roundToFrame(xToTime(mouse.x), sequence);

            if (_dragMode == DragMode::FadeIn) {
                clip->fadeIn = std::clamp<Time>(t - clip->start, 0, clip->duration - clip->fadeOut);
            }
            else {
                clip->fadeOut = std::clamp<Time>(clip->end() - t, 0, clip->duration - clip->fadeIn);
            }
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _dragMode = DragMode::None;
            _dragItems.clear();
            _grabbedClip = InvalidId;
            result.edited = true;
            result.label = "Change fade";
        }
    }
    else if (_dragMode == DragMode::Scrub) {
        playhead = std::max<Time>(0, snapToFrame(xToTime(std::max(mouse.x, _timelineLeft)), sequence.frameRate));
        _playhead = playhead;
        result.seeked = true;
    }
    else if (_dragMode == DragMode::Marquee) {
        // Started as a click: a rectangle once the mouse moved a little.
        const float startY = _marqueeY - _scrollY;
        const bool moved = std::abs(mouse.x - _marqueeX) > 4.f || std::abs(mouse.y - startY) > 4.f;

        if (moved) {
            _gap = Gap();

            const Time t0 = std::min(_marqueeTime, xToTime(mouse.x));
            const Time t1 = std::max(_marqueeTime, xToTime(mouse.x));
            const float y0 = std::min(startY, mouse.y);
            const float y1 = std::max(startY, mouse.y);

            selection = _marqueeBase;

            for (const Row& row : _rows) {
                if (row.top + row.height < y0 || row.top > y1) {
                    continue;
                }

                for (const Clip& clip : tracksOfType(sequence, row.type)[row.trackIndex].clips) {
                    if (clip.start < t1 && clip.end() > t0 && !isSelected(selection, clip.id)) {
                        selection.push_back(clip.id);
                    }
                }
            }

            const ImVec2 a(std::max(std::min(_marqueeX, mouse.x), _timelineLeft), std::max(y0, _tracksTop));
            const ImVec2 b(std::min(std::max(_marqueeX, mouse.x), _timelineRight), std::min(y1, _tracksBottom));
            drawList->AddRectFilled(a, b, IM_COL32(255, 122, 0, 40));
            drawList->AddRect(a, b, IM_COL32(255, 122, 0, 200));
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _dragMode = DragMode::None;
        }
    }
    else if (_dragMode == DragMode::Slip) {
        // The media under the clips, the other way of the mouse.
        const Time d = roundToFrame(xToTime(mouse.x) - xToTime(_dragStartX), sequence);
        bool changed = false;

        for (const DragItem& item : _dragItems) {
            Clip* clip = project.findClip(item.original.id);
            const MediaItem* media = clip ? project.findMedia(clip->mediaId) : nullptr;

            if (!clip || !media) {
                continue;
            }

            const Time maxIn = std::max<Time>(0, media->duration - clip->toSource(clip->duration));
            clip->sourceIn = std::clamp<Time>(item.original.sourceIn - clip->toSource(d), 0, maxIn);
            changed = changed || clip->sourceIn != item.original.sourceIn;
        }

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _dragMode = DragMode::None;
            _dragItems.clear();
            _grabbedClip = InvalidId;

            if (changed) {
                result.edited = true;
                result.label = "Slip clip";
            }
        }
    }
    else if (_dragMode != DragMode::None) {
        updateDrag(project, mouse.x, mouse.y);
    }

    if (_dragMode != DragMode::None && _dragMode != DragMode::Marquee && _dragMode != DragMode::Slip &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const DragMode mode = _dragMode;

        if (endDrag(project)) {
            result.edited = true;
            result.label = (mode == DragMode::Move) ? "Move clips" : "Trim clip";
        }
    }

    // ------------------------------------------------------------------------
    // Media dropped from the media bin
    // ------------------------------------------------------------------------

    if (ImGui::BeginDragDropTarget()) {
        // A sequence: nested where dropped, on a video track.
        if (const ImGuiPayload* sequencePayload = ImGui::AcceptDragDropPayload(SequencePayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            sequencePayload && hoveredRow && hoveredRow->type == TrackType::Video && mouse.x >= _timelineLeft) {
            const Id sequenceId = *(const Id*)sequencePayload->Data;
            const Sequence* nested = project.findSequence(sequenceId);
            const Time start = std::max<Time>(0, snapToFrame(snapTime(project, xToTime(mouse.x), false), sequence.frameRate));
            const bool allowed = nested && !project.sequenceContains(sequenceId, sequence.id);

            drawList->PushClipRect(ImVec2(_timelineLeft, _tracksTop), ImVec2(_timelineRight, _tracksBottom), true);
            drawList->AddRect(
                ImVec2(timeToX(start), hoveredRow->top + 2.f),
                ImVec2(timeToX(start + (nested && nested->duration() > 0 ? nested->duration() : 5 * TicksPerSecond)), hoveredRow->top + hoveredRow->height - 2.f),
                allowed ? IM_COL32(255, 255, 255, 180) : IM_COL32(230, 70, 60, 200), 3.f, 0, 2.f);
            drawList->PopClipRect();

            if (sequencePayload->IsDelivery() && allowed) {
                const Track& track = tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex];
                const Id clipId = project.addSequenceClip(sequenceId, track.id, start);

                if (clipId != InvalidId) {
                    selection = { clipId };
                    result.edited = true;
                    result.label = "Nest sequence";
                }
            }
        }

        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(MediaPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);

        if (payload && hoveredRow && mouse.x >= _timelineLeft) {
            const Id mediaId = *(const Id*)payload->Data;
            const Time start = std::max<Time>(0, snapToFrame(snapTime(project, xToTime(mouse.x), false), sequence.frameRate));
            const MediaItem* media = project.findMedia(mediaId);

            // Preview of the clip
            if (media) {
                drawList->PushClipRect(ImVec2(_timelineLeft, _tracksTop), ImVec2(_timelineRight, _tracksBottom), true);
                drawList->AddRect(
                    ImVec2(timeToX(start), hoveredRow->top + 2.f),
                    ImVec2(timeToX(start + media->duration), hoveredRow->top + hoveredRow->height - 2.f),
                    IM_COL32(255, 255, 255, 180), 3.f, 0, 2.f);
                drawList->PopClipRect();
            }

            if (payload->IsDelivery()) {
                const Track& track = tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex];
                const Id clipId = project.addClip(mediaId, track.id, start);

                if (clipId != InvalidId) {
                    selection.clear();
                    addWithLinks(project, clipId, selection);
                    result.edited = true;
                    result.label = "Add clip";
                }
            }
        }

        ImGui::EndDragDropTarget();
    }

    // Files dragged from the file manager: one after the other, where the
    // mouse is.
    if (_fileDrop.active && hovered && hoveredRow && mouse.x >= _timelineLeft) {
        const Time start = std::max<Time>(0, snapToFrame(snapTime(project, xToTime(mouse.x), false), sequence.frameRate));
        const float top = hoveredRow->top + 2.f;
        const float bottom = hoveredRow->top + hoveredRow->height - 2.f;

        drawList->PushClipRect(ImVec2(_timelineLeft, _tracksTop), ImVec2(_timelineRight, _tracksBottom), true);

        if (_fileDrop.durations.empty()) {
            // Lengths not known yet: where they start.
            drawList->AddLine(ImVec2(timeToX(start), top), ImVec2(timeToX(start), bottom), IM_COL32(255, 255, 255, 220), 2.f);
        }

        Time t = start;

        for (Time duration : _fileDrop.durations) {
            drawList->AddRectFilled(ImVec2(timeToX(t), top), ImVec2(timeToX(t + duration), bottom), IM_COL32(255, 255, 255, 30), 3.f);
            drawList->AddRect(ImVec2(timeToX(t), top), ImVec2(timeToX(t + duration), bottom), IM_COL32(255, 255, 255, 180), 3.f, 0, 2.f);
            t += duration;
        }

        drawList->PopClipRect();

        if (_fileDrop.dropped) {
            result.dropTrack = tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex].id;
            result.dropStart = start;
        }
    }

    _fileDrop = FileDrop();
    _liveRecordings.clear();

    // ------------------------------------------------------------------------
    // Context menu of the clips
    // ------------------------------------------------------------------------

    if (ImGui::BeginPopup("##clipContext")) {
        if (ImGui::MenuItem(tr(ICON_MD_CONTENT_CUT " Split at Playhead"), "Ctrl+K")) {
            project.splitAt(playhead, selection);
            result.edited = true;
            result.label = "Split clips";
        }

        if (ImGui::BeginMenu(tr(ICON_MD_COMPARE_ARROWS " Transition In"))) {
            for (int type = 0; type <= (int)TransitionType::Zoom; type++) {
                if (ImGui::MenuItem(tr(transitionName((TransitionType)type)))) {
                    for (Id clipId : selection) {
                        const Track* track = project.findTrackOfClip(clipId);
                        Clip* clip = project.findClip(clipId);

                        if (clip && track && track->type == TrackType::Video) {
                            clip->transitionIn.type = (TransitionType)type;
                        }
                    }

                    result.edited = true;
                    result.label = (type == 0) ? "Remove transition" : "Add transition";
                }
            }

            // Shaders: the built-in ones, then the user's.
            ImGui::Separator();

            auto shaderItem = [&](const ShaderDef& shader) {
                if (shader.kind != ShaderKind::Transition || !ImGui::MenuItem((std::string(ICON_MD_AUTO_AWESOME " ") + shader.name).c_str())) {
                    return;
                }

                for (Id clipId : selection) {
                    const Track* track = project.findTrackOfClip(clipId);
                    Clip* clip = project.findClip(clipId);

                    if (clip && track && track->type == TrackType::Video) {
                        clip->transitionIn.type = TransitionType::Shader;
                        clip->transitionIn.shader = shader.id;
                    }
                }

                if (shader.id.rfind("builtin:", 0) != 0) {
                    project.storeShader(shader);
                }

                result.edited = true;
                result.label = "Add transition";
            };

            for (const ShaderDef& shader : builtinShaders()) shaderItem(shader);

            if (userShaders) {
                for (const ShaderDef& shader : *userShaders) shaderItem(shader);
            }

            ImGui::EndMenu();
        }

        if (_contextTrack != InvalidId && ImGui::MenuItem(tr(ICON_MD_RECORD_VOICE_OVER " Generate Subtitles from this Track..."))) {
            result.generateSubtitles = _contextTrack;
        }

        if (ImGui::MenuItem(tr(ICON_MD_VOLUME_OFF " Remove the Sound of the Video"))) {
            // The clips of the sound tracks linked with the selected videos.
            std::vector<Id> sounds;

            for (Id clipId : selection) {
                const Clip* clip = project.findClip(clipId);

                if (!clip || clip->linkGroup == InvalidId) {
                    continue;
                }

                for (const Track& track : sequence.soundTracks) {
                    for (const Clip& sound : track.clips) {
                        if (sound.linkGroup == clip->linkGroup) sounds.push_back(sound.id);
                    }
                }
            }

            project.removeClips(sounds, false);
            std::erase_if(selection, [&](Id id) { return std::find(sounds.begin(), sounds.end(), id) != sounds.end(); });
            result.edited = true;
            result.label = "Remove sound";
        }

        if (ImGui::MenuItem(tr(ICON_MD_LINK_OFF " Unlink"))) {
            for (Id clipId : selection) {
                if (Clip* clip = project.findClip(clipId)) {
                    clip->linkGroup = InvalidId;
                }
            }

            result.edited = true;
            result.label = "Unlink clips";
        }

        ImGui::Separator();

        if (ImGui::MenuItem(tr(ICON_MD_DELETE " Delete"), "Del")) {
            project.removeClips(selection, false);
            selection.clear();
            result.edited = true;
            result.label = "Delete clips";
        }

        if (ImGui::MenuItem(tr(ICON_MD_DELETE_SWEEP " Ripple Delete"), "Shift+Del")) {
            project.rippleDelete(selection, false);
            selection.clear();
            result.edited = true;
            result.label = "Ripple delete";
        }

        ImGui::EndPopup();
    }

    // ------------------------------------------------------------------------
    // Ruler and playhead, over everything
    // ------------------------------------------------------------------------

    drawRuler(drawList, sequence, ImVec2(_timelineLeft, origin.y), ImVec2(_timelineRight, _tracksTop));

    const float playheadX = timeToX(playhead);

    if (playheadX >= _timelineLeft && playheadX <= _timelineRight) {
        const float half = 0.45f * unit;

        drawList->AddLine(ImVec2(playheadX, origin.y), ImVec2(playheadX, _tracksBottom), PlayheadColor, 1.5f);
        drawList->AddTriangleFilled(
            ImVec2(playheadX - half, _tracksTop - 1.1f * half),
            ImVec2(playheadX + half, _tracksTop - 1.1f * half),
            ImVec2(playheadX, _tracksTop),
            PlayheadColor);
    }

    // Header column corner, above the track headers.
    drawList->AddRectFilled(origin, ImVec2(_timelineLeft, _tracksTop), ImGui::GetColorU32(ImGuiCol_TitleBg));

    return result;
}


void TimelineView::drawRuler(ImDrawList* drawList, const Sequence& sequence, const ImVec2& min, const ImVec2& max) const
{
    const float unit = ImGui::GetFontSize();

    drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_TitleBg));
    drawList->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), ImGui::GetColorU32(ImGuiCol_Separator));

    // Labels need room for a timecode; frame steps are used when zoomed in
    // enough to see single frames.
    const float labelWidth = ImGui::CalcTextSize("00:00:00:00").x + unit;
    const double frameSeconds = 1.0 / sequence.frameRate.toDouble();

    double major = RulerSteps[std::size(RulerSteps) - 1];
    bool found = false;

    for (const double frames : { 1.0, 2.0, 5.0, 10.0 }) {
        if (frames * frameSeconds < 1.0 && frames * frameSeconds * _pixelsPerSecond >= labelWidth) {
            major = frames * frameSeconds;
            found = true;
            break;
        }
    }

    if (!found) {
        for (const double step : RulerSteps) {
            if (step * _pixelsPerSecond >= labelWidth) {
                major = step;
                break;
            }
        }
    }

    const double minor = major / 5.0;
    const bool drawMinor = minor * _pixelsPerSecond >= 0.35f * unit;

    const double firstSeconds = _scrollSeconds;
    const double lastSeconds = _scrollSeconds + (double)(max.x - min.x) / _pixelsPerSecond;

    drawList->PushClipRect(min, max, true);

    const ImU32 tickColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);

    if (drawMinor) {
        for (int64_t i = (int64_t)std::floor(firstSeconds / minor); i * minor <= lastSeconds; i++) {
            const float x = min.x + (float)((i * minor - _scrollSeconds) * _pixelsPerSecond);
            drawList->AddLine(ImVec2(x, max.y - 0.3f * unit), ImVec2(x, max.y), tickColor);
        }
    }

    for (int64_t i = (int64_t)std::floor(firstSeconds / major); i * major <= lastSeconds; i++) {
        const double seconds = i * major;
        const float x = min.x + (float)((seconds - _scrollSeconds) * _pixelsPerSecond);

        drawList->AddLine(ImVec2(x, max.y - 0.7f * unit), ImVec2(x, max.y), tickColor);

        const std::string label = formatTimecode(secondsToTime(seconds + 0.5 * frameSeconds), sequence.frameRate);
        drawList->AddText(ImVec2(x + 0.25f * unit, min.y + 0.1f * unit), textColor, label.c_str());
    }

    drawList->PopClipRect();
}


bool TimelineView::drawTrackHeaders(ImDrawList* drawList, Sequence& sequence, std::string& label, Id& effectsOfTrack)
{
    const float unit = ImGui::GetFontSize();
    const float buttonSize = 1.5f * unit;
    const bool canvasHovered = ImGui::IsItemHovered() && _dragMode == DragMode::None;
    bool edited = false;

    for (const Row& row : _rows) {
        Track& track = tracksOfType(sequence, row.type)[row.trackIndex];

        const ImVec2 min(_timelineLeft - _headerWidth, row.top + 1.f);
        const ImVec2 max(_timelineLeft - 1.f, row.top + row.height - 1.f);

        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_TitleBgActive));

        // Track type stripe
        const ImU32 stripe = (row.type == TrackType::Video) ? VideoClipColor : (row.type == TrackType::Subtitle ? SubtitleClipColor : AudioClipColor);
        drawList->AddRectFilled(min, ImVec2(min.x + 0.25f * unit, max.y), stripe);

        drawList->AddText(ImVec2(min.x + 0.6f * unit, min.y + 0.3f * unit), ImGui::GetColorU32(ImGuiCol_Text), track.name.c_str());

        // Buttons, right aligned on the first line.
        float x = max.x - 0.3f * unit - buttonSize;
        const float y = min.y + 0.2f * unit;

        if (headerButton(drawList, ImVec2(x, y), buttonSize, track.locked ? ICON_MD_LOCK : ICON_MD_LOCK_OPEN, track.locked, canvasHovered)) {
            track.locked = !track.locked;
            label = track.locked ? "Lock track" : "Unlock track";
            edited = true;
        }

        x -= buttonSize + 0.15f * unit;

        if (row.type == TrackType::Sound || row.type == TrackType::Audio) {
            // Effects, on the second line; highlighted when the track has some.
            if (headerButton(drawList, ImVec2(max.x - 0.3f * unit - buttonSize, y + buttonSize + 0.1f * unit), buttonSize,
                    ICON_MD_TUNE, !track.effects.empty(), canvasHovered)) {
                effectsOfTrack = track.id;
            }

            if (headerButton(drawList, ImVec2(x, y), buttonSize, ICON_MD_HEADSET, track.solo, canvasHovered)) {
                track.solo = !track.solo;
                label = "Solo track";
                edited = true;
            }

            x -= buttonSize + 0.15f * unit;
        }

        const bool picture = row.type == TrackType::Video || row.type == TrackType::Subtitle;
        const char* muteIcon = picture
            ? (track.muted ? ICON_MD_VISIBILITY_OFF : ICON_MD_VISIBILITY)
            : (track.muted ? ICON_MD_VOLUME_OFF : ICON_MD_VOLUME_UP);

        // Shown as active when the track is visible / audible.
        if (headerButton(drawList, ImVec2(x, y), buttonSize, muteIcon, !track.muted, canvasHovered)) {
            track.muted = !track.muted;
            label = picture ? "Toggle track output" : "Mute track";
            edited = true;
        }
    }

    return edited;
}


bool TimelineView::drawGroupHeaders(ImDrawList* drawList, Sequence& sequence, std::string& label)
{
    const float unit = ImGui::GetFontSize();
    const float buttonSize = 1.3f * unit;
    const bool canvasHovered = ImGui::IsItemHovered() && _dragMode == DragMode::None;
    bool edited = false;

    for (const GroupHeader& header : _groupHeaders) {
        const ImVec2 min(_timelineLeft - _headerWidth, header.top);
        const ImVec2 max(_timelineRight, header.top + header.height);

        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_TitleBg));
        drawList->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), ImGui::GetColorU32(ImGuiCol_Separator));

        const bool sound = header.type == TrackType::Sound;
        const float y = min.y + 0.5f * (header.height - buttonSize);
        float x = min.x + 0.2f * unit;

        // Fold / unfold
        if (sound) {
            const char* arrow = sequence.soundTracksFolded ? ICON_MD_CHEVRON_RIGHT : ICON_MD_EXPAND_MORE;

            if (headerButton(drawList, ImVec2(x, y), buttonSize, arrow, false, canvasHovered)) {
                sequence.soundTracksFolded = !sequence.soundTracksFolded;
            }

            x += buttonSize + 0.2f * unit;
        }

        // Mute / unmute the whole group, at the right of the header column;
        // the title before it, cut when too long (translations).
        const ImVec2 mutePosition(_timelineLeft - buttonSize - 0.3f * unit, y);
        const char* title = sound ? tr("Video sound") : (header.type == TrackType::Subtitle ? tr("Subtitles") : tr("Audio"));

        drawList->PushClipRect(ImVec2(x, min.y), ImVec2(mutePosition.x - 0.2f * unit, max.y), true);
        drawList->AddText(ImVec2(x, min.y + 0.5f * (header.height - ImGui::GetFontSize())), ImGui::GetColorU32(ImGuiCol_TextDisabled), title);
        drawList->PopClipRect();

        std::vector<Track>& tracks = sequence.tracks(header.type);
        const bool allMuted = !tracks.empty() && std::all_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.muted; });

        if (headerButton(drawList, mutePosition, buttonSize, allMuted ? ICON_MD_VOLUME_OFF : ICON_MD_VOLUME_UP, !allMuted, canvasHovered)) {
            for (Track& track : tracks) {
                track.muted = !allMuted;
            }

            label = allMuted ? "Unmute group" : "Mute group";
            edited = true;
        }
    }

    return edited;
}


void TimelineView::drawClips(ImDrawList* drawList, const Project& project, const std::vector<Id>& selection) const
{
    const float unit = ImGui::GetFontSize();
    const Sequence& sequence = project.sequence();

    for (const Row& row : _rows) {
        const Track& track = tracksOfType(sequence, row.type)[row.trackIndex];

        for (const Clip& clip : track.clips) {
            float x0 = timeToX(clip.start);
            float x1 = timeToX(clip.end());

            if (x1 < _timelineLeft || x0 > _timelineRight) {
                continue;
            }

            const bool selected = isSelected(selection, clip.id);
            const bool video = row.type == TrackType::Video;

            ImU32 color = video
                ? (selected ? VideoClipSelectedColor : VideoClipColor)
                : (selected ? AudioClipSelectedColor : AudioClipColor);

            if (row.type == TrackType::Subtitle) {
                color = selected ? SubtitleClipSelectedColor : SubtitleClipColor;
            }
            else if (clip.kind == ClipKind::Text) {
                color = selected ? TextClipSelectedColor : TextClipColor;
            }
            else if (clip.kind == ClipKind::Generator) {
                color = selected ? GeneratorClipSelectedColor : GeneratorClipColor;
            }
            else if (clip.kind == ClipKind::Sequence) {
                color = selected ? SequenceClipSelectedColor : SequenceClipColor;
            }
            else if (clip.frozen && !selected) {
                color = FrozenClipColor;
            }

            if (track.muted) {
                color = (color & 0x00FFFFFF) | 0x80000000;
            }

            const ImVec2 min(x0, row.top + 2.f);
            const ImVec2 max(std::max(x1, x0 + 1.f), row.top + row.height - 2.f);

            drawList->AddRectFilled(min, max, color, 3.f);

            // Pictures of a video along the clip, the time of each tile from
            // its left edge.
            if (video && clip.kind == ClipKind::Media && thumbnailsOf && max.y - min.y > 8.f) {
                const Thumbnails thumbnails = thumbnailsOf(clip.mediaId);

                if (thumbnails.ready > 0) {
                    const float tileWidth = (max.y - min.y - 2.f) * 16.f / 9.f;
                    const float left = std::max(min.x, _timelineLeft);
                    const float right = std::min(max.x, _timelineRight);
                    const ImU32 tint = (track.muted || clip.frozen) ? IM_COL32(255, 255, 255, 110) : IM_COL32(255, 255, 255, 200);

                    drawList->PushClipRect(ImVec2(left, min.y), ImVec2(right, max.y), true);

                    for (float x = min.x + std::floor((left - min.x) / tileWidth) * tileWidth; x < right; x += tileWidth) {
                        const Time source = clip.sourceAt(std::max<Time>(0, xToTime(x) - clip.start));
                        int index = thumbnails.image ? 0 : (int)std::clamp<Time>((source + thumbnails.interval / 2) / std::max<Time>(1, thumbnails.interval), 0, thumbnails.count - 1);

                        if (index >= thumbnails.ready) {
                            continue;
                        }

                        const float u = (float)(index % thumbnails.columns) * thumbnails.tileU;
                        const float v = (float)(index / thumbnails.columns) * thumbnails.tileV;

                        drawList->AddImage(thumbnails.texture, ImVec2(x, min.y + 1.f), ImVec2(x + tileWidth, max.y - 1.f),
                            ImVec2(u, v), ImVec2(u + thumbnails.tileU, v + thumbnails.tileV), tint);
                    }

                    // The name stays readable.
                    drawList->AddRectFilledMultiColor(min, ImVec2(max.x, min.y + 1.3f * unit),
                        IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
                    drawList->PopClipRect();
                }
            }

            // Audio fades: the curve, and the handles at the top corners.
            const bool sound = row.type == TrackType::Sound || row.type == TrackType::Audio;

            if (sound) {
                const float fadeInX = std::min(timeToX(clip.start + clip.fadeIn), max.x);
                const float fadeOutX = std::max(timeToX(clip.end() - clip.fadeOut), min.x);
                const ImU32 fadeColor = IM_COL32(0, 0, 0, 90);

                if (clip.fadeIn > 0) {
                    drawList->AddTriangleFilled(ImVec2(min.x, min.y), ImVec2(fadeInX, min.y), ImVec2(min.x, max.y), fadeColor);
                    drawList->AddLine(ImVec2(min.x, max.y), ImVec2(fadeInX, min.y), IM_COL32(255, 255, 255, 160), 1.f);
                }

                if (clip.fadeOut > 0) {
                    drawList->AddTriangleFilled(ImVec2(fadeOutX, min.y), ImVec2(max.x, min.y), ImVec2(max.x, max.y), fadeColor);
                    drawList->AddLine(ImVec2(fadeOutX, min.y), ImVec2(max.x, max.y), IM_COL32(255, 255, 255, 160), 1.f);
                }

                if (selected && max.x - min.x > 3.f * unit) {
                    const float r = 0.22f * unit;
                    drawList->AddCircleFilled(ImVec2(std::max(fadeInX, min.x + r), min.y + r + 1.f), r, IM_COL32(255, 255, 255, 230));
                    drawList->AddCircleFilled(ImVec2(std::min(fadeOutX, max.x - r), min.y + r + 1.f), r, IM_COL32(255, 255, 255, 230));
                }
            }

            if (sound && waveformOf) {
                if (const std::shared_ptr<const media::Waveform> waveform = waveformOf(clip.mediaId)) {
                    // Below the name of the clip.
                    const ImVec2 waveMin(min.x, min.y + 1.2f * unit);
                    drawWaveform(drawList, clip, *waveform, waveMin, ImVec2(max.x, max.y - 0.15f * unit));
                }
            }

            drawList->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 220) : IM_COL32(0, 0, 0, 120), 3.f, 0, selected ? 2.f : 1.f);

            // Name, kept visible when the clip starts before the view.
            const MediaItem* media = project.findMedia(clip.mediaId);

            if ((media || clip.kind != ClipKind::Media) && max.x - min.x > 2.f * unit) {
                std::string name;

                if (clip.kind == ClipKind::Text) {
                    name = ICON_MD_TEXT_FIELDS " " + clip.text.plainText();
                    std::replace(name.begin(), name.end(), '\n', ' ');
                }
                else if (clip.kind == ClipKind::Generator) {
                    const ShaderDef* shader = project.findShader(clip.shader);
                    name = std::string(ICON_MD_AUTO_AWESOME " ") + (shader ? shader->name : clip.shader);
                }
                else if (clip.kind == ClipKind::Sequence) {
                    const Sequence* nested = project.findSequence(clip.sequence);
                    name = std::string(ICON_MD_VIDEO_LIBRARY " ") + (nested ? nested->name : "?");
                }
                else {
                    name = clip.frozen ? std::string(ICON_MD_PAUSE_CIRCLE " ") + media->name() : media->name();

                    if (clip.speed != 1.0) {
                        char speed[32];
                        std::snprintf(speed, sizeof(speed), "  " ICON_MD_SPEED " %g%%", std::round(clip.speed * 1000.0) / 10.0);
                        name += speed;
                    }
                }
                const float textX = std::max(min.x, _timelineLeft) + 0.35f * unit;

                drawList->PushClipRect(min, ImVec2(max.x - 0.2f * unit, max.y), true);
                drawList->AddText(ImVec2(textX, min.y + 0.2f * unit), IM_COL32(255, 255, 255, 230), name.c_str());

                if (media && clip.linkGroup == InvalidId && media->type == MediaType::Video && !clip.frozen) {
                    drawList->AddText(ImVec2(textX, min.y + 1.3f * unit), IM_COL32(255, 255, 255, 140), ICON_MD_LINK_OFF);
                }

                drawList->PopClipRect();
            }
        }
    }

    // Recordings being made
    for (const LiveRecording& live : _liveRecordings) {
        for (const Row& row : _rows) {
            if (tracksOfType(sequence, row.type)[row.trackIndex].id != live.trackId) {
                continue;
            }

            const ImVec2 min(timeToX(live.start), row.top + 2.f);
            const ImVec2 max(std::max(timeToX(live.start + live.length), min.x + 1.f), row.top + row.height - 2.f);

            drawList->PushClipRect(ImVec2(_timelineLeft, _tracksTop), ImVec2(_timelineRight, _tracksBottom), true);
            drawList->AddRectFilled(min, max, IM_COL32(170, 50, 45, 255), 3.f);

            if (live.waveform) {
                Clip clip;
                clip.start = live.start;
                clip.duration = std::max<Time>(1, live.length);
                drawWaveform(drawList, clip, *live.waveform, ImVec2(min.x, min.y + 1.2f * unit), ImVec2(max.x, max.y - 0.15f * unit));
            }

            drawList->AddRect(min, max, IM_COL32(255, 120, 110, 230), 3.f, 0, 1.5f);

            if (max.x - min.x > 2.f * unit) {
                drawList->PushClipRect(min, ImVec2(max.x - 0.2f * unit, max.y), true);
                drawList->AddText(ImVec2(std::max(min.x, _timelineLeft) + 0.35f * unit, min.y + 0.2f * unit),
                    IM_COL32(255, 255, 255, 230), tr(ICON_MD_FIBER_MANUAL_RECORD " Recording"));
                drawList->PopClipRect();
            }

            drawList->PopClipRect();
        }
    }

    // Selected gap
    if (hasGap()) {
        for (const Row& row : _rows) {
            if (tracksOfType(sequence, row.type)[row.trackIndex].id != _gap.trackId) {
                continue;
            }

            const ImVec2 min(std::max(timeToX(_gap.start), _timelineLeft), row.top + 2.f);
            const ImVec2 max(timeToX(_gap.end), row.top + row.height - 2.f);

            drawList->AddRectFilled(min, max, IM_COL32(255, 122, 0, 60), 3.f);
            drawList->AddRect(min, max, IM_COL32(255, 122, 0, 200), 3.f, 0, 1.5f);
        }
    }

    // Transitions, centered on the cuts.
    for (const Row& row : _rows) {
        if (row.type != TrackType::Video) {
            continue;
        }

        const Track& track = tracksOfType(sequence, row.type)[row.trackIndex];

        for (const Clip& clip : track.clips) {
            if (clip.transitionIn.type == TransitionType::None) {
                continue;
            }

            const Time half = std::min(clip.transitionIn.duration, clip.duration) / 2;
            const float x0 = timeToX(clip.start - half);
            const float x1 = std::max(timeToX(clip.start + half), x0 + 0.8f * unit);
            const ImVec2 min(x0, row.top + 0.35f * row.height);
            const ImVec2 max(x1, row.top + row.height - 3.f);

            drawList->AddRectFilled(min, max, IM_COL32(255, 200, 90, 150), 3.f);
            drawList->AddRect(min, max, IM_COL32(255, 220, 140, 230), 3.f);

            const ImVec2 iconSize = ImGui::CalcTextSize(ICON_MD_COMPARE_ARROWS);

            if (max.x - min.x > iconSize.x) {
                drawList->AddText(ImVec2(0.5f * (min.x + max.x - iconSize.x), 0.5f * (min.y + max.y - iconSize.y)),
                    IM_COL32(40, 30, 10, 255), ICON_MD_COMPARE_ARROWS);
            }
        }
    }

    // Snap guide while dragging: the playhead or the clip edge reached.
    if (_dragMode != DragMode::None && _dragMode != DragMode::Scrub && _snapLine >= 0) {
        const float x = timeToX(_snapLine);
        drawList->AddLine(ImVec2(x, _tracksTop), ImVec2(x, _tracksBottom), SnapLineColor, 1.f);
    }
}


void TimelineView::drawWaveform(ImDrawList* drawList, const Clip& clip, const media::Waveform& waveform, const ImVec2& min, const ImVec2& max) const
{
    if (waveform.levels.empty() || max.y - min.y < 4.f) {
        return;
    }

    const double rate = (double)waveform.sampleRate;
    // Frames of the source per pixel: more when the clip plays faster.
    const double framesPerPixel = rate * clip.speed / _pixelsPerSecond;

    // The coarsest level still finer than a pixel.
    size_t level = 0;

    while (level + 1 < waveform.levels.size() && (double)media::Waveform::blockSize(level + 1) <= framesPerPixel) {
        level++;
    }

    const std::vector<media::Waveform::Peak>& peaks = waveform.levels[level];
    const int64_t blockSize = media::Waveform::blockSize(level);

    const float centerY = 0.5f * (min.y + max.y);
    const float halfHeight = 0.5f * (max.y - min.y);
    const float gain = std::pow(10.f, clip.gainDb / 20.f);
    const ImU32 color = IM_COL32(210, 255, 225, 170);

    // Source frame at the left edge of the clip.
    const double sourceStart = timeToSeconds(clip.sourceIn) * rate;
    const float left = std::max(min.x, _timelineLeft);
    const float right = std::min(max.x, _timelineRight);

    for (float x = std::floor(left); x < right; x += 1.f) {
        const double f0 = sourceStart + (double)(x - min.x) * framesPerPixel;
        const double f1 = f0 + std::max(1.0, framesPerPixel);

        int64_t first = (int64_t)(f0 / blockSize);
        int64_t last = std::max(first + 1, (int64_t)(f1 / blockSize));

        first = std::max<int64_t>(0, first);
        last = std::min<int64_t>((int64_t)peaks.size(), last);

        if (first >= last) {
            continue;
        }

        float low = 1.f;
        float high = -1.f;

        for (int64_t i = first; i < last; i++) {
            low = std::min(low, peaks[i].min);
            high = std::max(high, peaks[i].max);
        }

        const float top = centerY - displayLevel(high * gain) * halfHeight;
        const float bottom = centerY - displayLevel(low * gain) * halfHeight;

        drawList->AddRectFilled(ImVec2(x, top), ImVec2(x + 1.f, std::max(bottom, top + 1.f)), color);
    }
}


bool TimelineView::toolPressed(Project& project, const Clip* hoveredClip, const Row* hoveredRow, const ImVec2& mouse, std::vector<Id>& selection)
{
    const ImGuiIO& io = ImGui::GetIO();
    Sequence& sequence = project.sequence();
    const Track* track = hoveredClip ? project.findTrackOfClip(hoveredClip->id) : nullptr;

    switch (_tool) {
        case Tool::Blade: {
            if (!hoveredClip || !track || track->locked) {
                return false;
            }

            // Shift: across all the tracks.
            const Time t = snapToFrame(snapTime(project, xToTime(mouse.x), false), sequence.frameRate);

            if (!io.KeyShift && (t <= hoveredClip->start || t >= hoveredClip->end())) {
                return false;
            }

            const std::string before = project.toJson({}).dump(0);
            project.splitAt(t, io.KeyShift ? std::vector<Id>() : std::vector<Id>{ hoveredClip->id });

            return project.toJson({}).dump(0) != before;
        }

        case Tool::Slip: {
            const MediaItem* media = hoveredClip ? project.findMedia(hoveredClip->mediaId) : nullptr;

            if (!hoveredClip || !track || track->locked || !media || media->type == MediaType::Image ||
                hoveredClip->kind != ClipKind::Media || hoveredClip->frozen) {
                return false;
            }

            selection.clear();
            addWithLinks(project, hoveredClip->id, selection);

            _dragMode = DragMode::Slip;
            _dragItems.clear();
            _grabbedClip = hoveredClip->id;
            _dragStartX = mouse.x;

            for (Id id : selection) {
                const Clip* clip = project.findClip(id);
                Track* clipTrack = project.findTrackOfClip(id);
                const MediaItem* clipMedia = clip ? project.findMedia(clip->mediaId) : nullptr;

                // The sound of a video slips with it.
                if (clip && clipTrack && !clipTrack->locked && clip->kind == ClipKind::Media && !clip->frozen &&
                    clipMedia && clipMedia->type != MediaType::Image) {
                    const std::vector<Track>& list = tracksOfType(sequence, clipTrack->type);
                    _dragItems.push_back({ *clip, clipTrack->type, (int)(clipTrack - list.data()) });
                }
            }

            return false;
        }

        case Tool::SelectForward: {
            // The clips from the click on: all the tracks, or this one
            // (Shift), with their links.
            const Time t = hoveredClip ? hoveredClip->start : xToTime(mouse.x);
            const Track* onlyTrack = (io.KeyShift && hoveredRow) ? &tracksOfType(sequence, hoveredRow->type)[hoveredRow->trackIndex] : nullptr;

            if (!io.KeyCtrl) {
                selection.clear();
            }

            for (const Row& row : _rows) {
                const Track& rowTrack = tracksOfType(sequence, row.type)[row.trackIndex];

                if ((onlyTrack && &rowTrack != onlyTrack) || rowTrack.locked) {
                    continue;
                }

                for (const Clip& clip : rowTrack.clips) {
                    if (clip.start >= t && !isSelected(selection, clip.id)) {
                        addWithLinks(project, clip.id, selection);
                    }
                }
            }

            if (hoveredClip && track && !track->locked && isSelected(selection, hoveredClip->id)) {
                const Clip grabbed = *hoveredClip;
                beginDrag(project, DragMode::Move, grabbed, selection);
            }

            return false;
        }

        default:
            return false;
    }
}


void TimelineView::beginDrag(Project& project, DragMode mode, const Clip& grabbed, const std::vector<Id>& selection)
{
    Sequence& sequence = project.sequence();

    if (mode == DragMode::FadeIn || mode == DragMode::FadeOut) {
        _dragMode = mode;
        _dragItems.clear();
        _grabbedClip = grabbed.id;
        return;
    }

    _dragMode = mode;
    _dragItems.clear();
    _grabbedClip = grabbed.id;
    _dragStartX = ImGui::GetIO().MousePos.x;
    _dragStartY = ImGui::GetIO().MousePos.y;
    _lastValidDelta = 0;
    _lastValidTrackDelta = 0;
    _snapLine = -1;

    // A move takes the whole selection; a trim only the grabbed clip and the
    // clips linked to it.
    std::vector<Id> clipIds;

    if (mode == DragMode::Move) {
        clipIds = selection;
    }
    else {
        addWithLinks(project, grabbed.id, clipIds);
    }

    for (const TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio, TrackType::Subtitle }) {
        const std::vector<Track>& tracks = tracksOfType(sequence, type);

        for (int i = 0; i < (int)tracks.size(); i++) {
            for (const Clip& clip : tracks[i].clips) {
                if (std::find(clipIds.begin(), clipIds.end(), clip.id) == clipIds.end()) {
                    continue;
                }

                // Clips of locked tracks stay where they are.
                if (tracks[i].locked) {
                    continue;
                }

                _dragItems.push_back({ clip, type, i });

                if (clip.id == grabbed.id) {
                    _grabbedType = type;
                    _grabbedTrackIndex = i;
                }
            }
        }
    }

    if (_dragItems.empty()) {
        _dragMode = DragMode::None;
    }
}


Time TimelineView::snapTime(const Project& project, Time t, bool excludeDragged) const
{
    if (!_snapping || ImGui::GetIO().KeyAlt) {
        return t;
    }

    const Time threshold = secondsToTime((0.6 * ImGui::GetFontSize()) / _pixelsPerSecond);
    Time best = t;
    Time bestDistance = threshold + 1;

    auto consider = [&](Time candidate) {
        const Time distance = std::abs(candidate - t);

        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    };

    consider(0);
    consider(_playhead);

    for (const auto* tracks : { &project.sequence().videoTracks, &project.sequence().soundTracks, &project.sequence().audioTracks, &project.sequence().subtitleTracks }) {
        for (const Track& track : *tracks) {
            for (const Clip& clip : track.clips) {
                const bool dragged = excludeDragged && std::any_of(_dragItems.begin(), _dragItems.end(),
                    [&](const DragItem& item) { return item.original.id == clip.id; });

                if (!dragged) {
                    consider(clip.start);
                    consider(clip.end());
                }
            }
        }
    }

    return best;
}


void TimelineView::updateDrag(Project& project, float mouseX, float mouseY)
{
    Sequence& sequence = project.sequence();
    const Time frame = frameDuration(sequence);

    // Before the mouse really moved, a click is just a selection.
    if (_lastValidDelta == 0 && _lastValidTrackDelta == 0 &&
        std::abs(mouseX - _dragStartX) < 3.f && std::abs(mouseY - _dragStartY) < 3.f) {
        return;
    }

    const DragItem* grabbed = nullptr;

    for (const DragItem& item : _dragItems) {
        if (item.original.id == _grabbedClip) grabbed = &item;
    }

    if (!grabbed) {
        return;
    }

    // Time offset, in whole frames.
    Time delta = roundToFrame(secondsToTime((double)(mouseX - _dragStartX) / _pixelsPerSecond), sequence);

    // Snapping of the dragged edge(s) of the grabbed clip.
    {
        std::vector<Time> edges;

        if (_dragMode == DragMode::Move || _dragMode == DragMode::TrimStart) edges.push_back(grabbed->original.start + delta);
        if (_dragMode == DragMode::Move || _dragMode == DragMode::TrimEnd)   edges.push_back(grabbed->original.end() + delta);

        Time bestAdjust = 0;
        bool snapped = false;
        _snapLine = -1;

        // Edges are removed from the tracks during the drag: exclude the
        // dragged clips explicitly.
        for (const Time edge : edges) {
            const Time target = snapTime(project, edge, true);

            if (target != edge && (!snapped || std::abs(target - edge) < std::abs(bestAdjust))) {
                bestAdjust = target - edge;
                snapped = true;
                _snapLine = target;
            }
        }

        delta += bestAdjust;
    }

    // Limits of the offset.
    Time minDelta = std::numeric_limits<Time>::min() / 4;
    Time maxDelta = std::numeric_limits<Time>::max() / 4;

    for (const DragItem& item : _dragItems) {
        const Clip& clip = item.original;
        const MediaItem* media = project.findMedia(clip.mediaId);
        const bool limited = media && media->type != MediaType::Image && !clip.frozen;

        switch (_dragMode) {
            case DragMode::Move:
                minDelta = std::max(minDelta, -clip.start);
                break;
            case DragMode::TrimStart:
                minDelta = std::max({ minDelta, -clip.start, limited ? -clip.fromSource(clip.sourceIn) : minDelta });
                maxDelta = std::min(maxDelta, clip.duration - frame);
                break;
            case DragMode::TrimEnd:
                minDelta = std::max(minDelta, -(clip.duration - frame));

                if (limited) {
                    maxDelta = std::min(maxDelta, clip.fromSource(media->duration - clip.sourceIn) - clip.duration);
                }
                break;
            default:
                break;
        }
    }

    delta = (minDelta <= maxDelta) ? std::clamp(delta, minDelta, maxDelta) : 0;

    // Track offset, for the clips of the type of the grabbed one.
    int trackDelta = _lastValidTrackDelta;

    if (_dragMode == DragMode::Move) {
        const Row* row = rowAt(mouseY);

        if (row && groupOf(row->type) == groupOf(_grabbedType)) {
            trackDelta = row->trackIndex - _grabbedTrackIndex;
        }
    }

    // Take the dragged clips off their tracks, then place them again.
    for (const DragItem& item : _dragItems) {
        for (const TrackType type : { TrackType::Video, TrackType::Sound, TrackType::Audio, TrackType::Subtitle }) {
            for (Track& track : tracksOfType(sequence, type)) {
                std::erase_if(track.clips, [&](const Clip& clip) { return clip.id == item.original.id; });
            }
        }
    }

    auto place = [&](Time d, int td, bool force) {
        std::vector<std::pair<Track*, Clip>> placed;

        for (const DragItem& item : _dragItems) {
            std::vector<Track>& tracks = tracksOfType(sequence, item.trackType);
            const int index = item.originalTrackIndex + ((groupOf(item.trackType) == groupOf(_grabbedType)) ? td : 0);

            if (index < 0 || index >= (int)tracks.size() || (tracks[index].locked && !force)) {
                return false;
            }

            Clip clip = item.original;

            switch (_dragMode) {
                case DragMode::Move:
                    clip.start += d;
                    break;
                case DragMode::TrimStart:
                    clip.start += d;
                    clip.duration -= d;

                    // A still shows the same frame whatever its start; the
                    // animation stays where it was in time.
                    if (!clip.frozen) {
                        clip.sourceIn = std::max<Time>(0, clip.sourceIn + clip.toSource(d));
                    }

                    clip.transform.shiftKeys(-d);
                    break;
                case DragMode::TrimEnd:
                    clip.duration += d;
                    break;
                default:
                    break;
            }

            if (!force) {
                for (const Clip& other : tracks[index].clips) {
                    if (clip.start < other.end() && other.start < clip.end()) {
                        return false;
                    }
                }
            }

            placed.emplace_back(&tracks[index], clip);
        }

        for (auto& [track, clip] : placed) {
            track->clips.push_back(clip);
            track->sortClips();
        }

        return true;
    };

    if (place(delta, trackDelta, false)) {
        _lastValidDelta = delta;
        _lastValidTrackDelta = trackDelta;
    }
    else if (!place(_lastValidDelta, _lastValidTrackDelta, false)) {
        // Cannot happen: the last placement was valid and nothing else moved.
        place(0, 0, true);
        _lastValidDelta = 0;
        _lastValidTrackDelta = 0;
    }
}


bool TimelineView::endDrag(Project& project)
{
    const DragMode mode = _dragMode;
    _dragMode = DragMode::None;

    if (mode == DragMode::Scrub) {
        return false;
    }

    const bool changed = _lastValidDelta != 0 || _lastValidTrackDelta != 0;

    _dragItems.clear();
    _grabbedClip = InvalidId;
    _lastValidDelta = 0;
    _lastValidTrackDelta = 0;

    return changed;
}
