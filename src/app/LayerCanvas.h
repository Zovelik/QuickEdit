#pragma once

#include <core/Project.h>

#include <imgui.h>

#include <array>
#include <vector>


// Mouse editing of layers placed in a frame: the viewer of the timeline
// (clips) and the frame of the recording scenes (sources) share it, so both
// behave the same.
//
// - A click picks the top layer under the mouse (Ctrl: the one below the
//   selected one), a drag moves it. It snaps to the edges and the middle of
//   the frame and of the other layers (Alt: freely), with guides.
// - The corners of the selected layer scale it around its center (snaps to
//   100 % and to the edges of the frame, Ctrl: freely), the handle above it
//   rotates it (snaps to right angles, Shift: steps of 15 degrees, Ctrl:
//   freely), the middles of its edges crop it, Alt + a corner crops two
//   edges (as in OBS).
//
// Positions are in pixels of the frame (of the sequence), its top left at
// (0, 0).
class LayerCanvas
{
public:
    struct Layer
    {
        core::Id id = core::InvalidId;

        // The quad shown, in pixels of the frame: top left, top right,
        // bottom right, bottom left of the layer before its rotation.
        std::array<ImVec2, 4> corners {};

        // The transform: center from the center of the frame, scale, degrees.
        float x = 0.f;
        float y = 0.f;
        float scale = 1.f;
        float rotation = 0.f;

        // The picture at a scale of 1 (fitted in the frame), in pixels of
        // the frame: 0 when it cannot be cropped (texts...).
        float halfWidth = 0.f;
        float halfHeight = 0.f;

        // Fractions of the picture cut from each edge.
        float cropLeft = 0.f;
        float cropTop = 0.f;
        float cropRight = 0.f;
        float cropBottom = 0.f;

        bool movable = true;            // False: picked, but neither moved nor given handles
        bool rotatable = true;
    };

    enum class Drag
    {
        None,
        Move,
        Scale,
        Rotate,
        Crop,
    };

    struct Result
    {
        core::Id clicked = core::InvalidId;     // Picked by a click (selection to update)
        bool clickedEmpty = false;              // A click on no layer
        bool doubleClicked = false;
        bool dragStarted = false;

        // The dragged layer, changed this frame: its new placement.
        bool edited = false;
        Layer layer;

        // Released after a change: one step of history.
        bool finished = false;
        Drag drag = Drag::None;
    };

    // Layers top first; the selection decides the outlines, its first layer
    // gets the handles. `hovered`: the frame is under the mouse (its
    // invisible button).
    Result update(const ImVec2& frameMin, const ImVec2& frameSize, float frameWidth, float frameHeight,
                  const std::vector<Layer>& layers, const std::vector<core::Id>& selection, bool hovered);

    // Outlines, handles and guides, after update().
    void draw(ImDrawList* drawList, const ImVec2& frameMin, const ImVec2& frameSize, float frameWidth,
              const std::vector<Layer>& layers, const std::vector<core::Id>& selection) const;

    bool dragging() const { return _drag != Drag::None; }
    core::Id dragged() const { return _drag != Drag::None ? _dragged : core::InvalidId; }
    void cancel() { _drag = Drag::None; _guides.clear(); }

private:
    struct Guide
    {
        bool vertical = true;
        float at = 0.f;                 // Pixels of the frame
    };

    // Handles of a layer, in pixels of the frame.
    struct Handles
    {
        const Layer* layer = nullptr;
        ImVec2 center;
        ImVec2 rotation;
        ImVec2 edges[4];                // Top, right, bottom, left
    };

    Handles handlesOf(const Layer& layer, float toFrame) const;
    int handleAt(const Handles& handles, const ImVec2& point, float toFrame) const;

    Drag _drag = Drag::None;
    core::Id _dragged = core::InvalidId;
    Layer _start;                       // The dragged layer when the drag started
    ImVec2 _startMouse;
    ImVec2 _center;
    bool _cropLeft = false;
    bool _cropRight = false;
    bool _cropTop = false;
    bool _cropBottom = false;
    ImGuiMouseCursor _cursor = ImGuiMouseCursor_Arrow;
    bool _moved = false;
    int _hoveredHandle = -1;
    bool _cropKey = false;
    std::vector<Guide> _guides;
};
