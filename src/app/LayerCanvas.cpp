#include <app/LayerCanvas.h>

#include <fonts/IconsMaterialDesign.h>

#include <algorithm>
#include <cmath>


namespace {

constexpr float Pi = 3.14159265358979f;
constexpr float HandleRadius = 6.f;     // Screen pixels
constexpr float SnapDistance = 8.f;     // Screen pixels

const ImU32 SelectedColor = IM_COL32(255, 122, 0, 255);
const ImU32 CropColor = IM_COL32(60, 170, 255, 255);
const ImU32 GuideColor = IM_COL32(255, 60, 200, 230);


bool inside(const std::array<ImVec2, 4>& quad, const ImVec2& p)
{
    // Convex quad: on the same side of its four edges.
    int sign = 0;

    for (int i = 0; i < 4; i++) {
        const ImVec2& a = quad[(size_t)i];
        const ImVec2& b = quad[(size_t)(i + 1) % 4];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        const int s = (cross > 0.f) ? 1 : (cross < 0.f ? -1 : 0);

        if (s != 0 && sign != 0 && s != sign) {
            return false;
        }

        if (s != 0) {
            sign = s;
        }
    }

    return true;
}


// Bounding box of a quad.
void boundsOf(const std::array<ImVec2, 4>& quad, ImVec2& min, ImVec2& max)
{
    min = max = quad[0];

    for (const ImVec2& p : quad) {
        min = ImVec2(std::min(min.x, p.x), std::min(min.y, p.y));
        max = ImVec2(std::max(max.x, p.x), std::max(max.y, p.y));
    }
}


// The arrows of a resize cursor along a direction on screen.
ImGuiMouseCursor resizeCursor(const ImVec2& direction)
{
    float angle = std::atan2(direction.y, direction.x) * 180.f / Pi;
    angle = std::fmod(angle + 360.f, 180.f);

    return (angle < 22.5f || angle >= 157.5f) ? ImGuiMouseCursor_ResizeEW
        : (angle < 67.5f) ? ImGuiMouseCursor_ResizeNWSE
        : (angle < 112.5f) ? ImGuiMouseCursor_ResizeNS
        : ImGuiMouseCursor_ResizeNESW;
}


// Rotation: no such cursor in the system ones, a drawn one.
void rotationCursor()
{
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    ImDrawList* foreground = ImGui::GetForegroundDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 size = ImGui::CalcTextSize(ICON_MD_ROTATE_RIGHT);
    const ImVec2 at(mouse.x - 0.5f * size.x, mouse.y - 0.5f * size.y);

    for (const ImVec2& offset : { ImVec2(-1.f, 0.f), ImVec2(1.f, 0.f), ImVec2(0.f, -1.f), ImVec2(0.f, 1.f) }) {
        foreground->AddText(ImVec2(at.x + offset.x, at.y + offset.y), IM_COL32(0, 0, 0, 220), ICON_MD_ROTATE_RIGHT);
    }

    foreground->AddText(at, IM_COL32(255, 255, 255, 255), ICON_MD_ROTATE_RIGHT);
}


// Pixels of the frame to the pixels of the picture of a layer (centered,
// before its transform), and back.
ImVec2 toLayer(const LayerCanvas::Layer& layer, float frameWidth, float frameHeight, const ImVec2& p)
{
    const float angle = layer.rotation * Pi / 180.f;
    const float scale = std::max(1e-3f, layer.scale);
    const float dx = p.x - (0.5f * frameWidth + layer.x);
    const float dy = p.y - (0.5f * frameHeight + layer.y);

    return ImVec2((dx * std::cos(angle) + dy * std::sin(angle)) / scale, (-dx * std::sin(angle) + dy * std::cos(angle)) / scale);
}


ImVec2 fromLayer(const LayerCanvas::Layer& layer, float frameWidth, float frameHeight, const ImVec2& p)
{
    const float angle = layer.rotation * Pi / 180.f;
    const float sx = p.x * layer.scale;
    const float sy = p.y * layer.scale;

    return ImVec2(0.5f * frameWidth + layer.x + sx * std::cos(angle) - sy * std::sin(angle),
                  0.5f * frameHeight + layer.y + sx * std::sin(angle) + sy * std::cos(angle));
}

} // namespace


LayerCanvas::Handles LayerCanvas::handlesOf(const Layer& layer, float toFrame) const
{
    Handles handles;
    handles.layer = &layer;

    const auto& c = layer.corners;
    handles.center = ImVec2(0.25f * (c[0].x + c[1].x + c[2].x + c[3].x), 0.25f * (c[0].y + c[1].y + c[2].y + c[3].y));

    for (int i = 0; i < 4; i++) {
        handles.edges[i] = ImVec2(0.5f * (c[(size_t)i].x + c[(size_t)(i + 1) % 4].x), 0.5f * (c[(size_t)i].y + c[(size_t)(i + 1) % 4].y));
    }

    // Above the middle of the top edge, away from the center.
    const ImVec2 top = handles.edges[0];
    const ImVec2 up(top.x - handles.center.x, top.y - handles.center.y);
    const float length = std::max(1.f, std::sqrt(up.x * up.x + up.y * up.y));
    const float offset = 28.f * toFrame;
    handles.rotation = ImVec2(top.x + up.x / length * offset, top.y + up.y / length * offset);

    return handles;
}


// 0..3: corners, 4: rotation, 5..8: edges (top, right, bottom, left), -1: none.
int LayerCanvas::handleAt(const Handles& handles, const ImVec2& point, float toFrame) const
{
    const float grab = 1.5f * HandleRadius * toFrame;
    const Layer& layer = *handles.layer;
    const bool croppable = layer.halfWidth > 0.f && layer.halfHeight > 0.f;
    int handle = -1;

    auto near = [&](const ImVec2& p) {
        return std::abs(point.x - p.x) <= grab && std::abs(point.y - p.y) <= grab;
    };

    for (int i = 0; i < 4; i++) {
        if (near(layer.corners[(size_t)i])) handle = i;
    }

    for (int i = 0; i < 4; i++) {
        if (croppable && near(handles.edges[i])) handle = 5 + i;
    }

    if (layer.rotatable && near(handles.rotation)) {
        handle = 4;
    }

    return handle;
}


LayerCanvas::Result LayerCanvas::update(const ImVec2& frameMin, const ImVec2& frameSize, float frameWidth, float frameHeight,
                                        const std::vector<Layer>& layers, const std::vector<core::Id>& selection, bool hovered)
{
    Result result;
    const float toFrame = frameWidth / std::max(1.f, frameSize.x);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 point((mouse.x - frameMin.x) * toFrame, (mouse.y - frameMin.y) * toFrame);
    const ImGuiIO& io = ImGui::GetIO();

    // The layer with handles: the first selected one, when it moves.
    const Layer* handleLayer = nullptr;

    if (!selection.empty()) {
        for (const Layer& layer : layers) {
            if (layer.id == selection.front() && layer.movable) {
                handleLayer = &layer;
                break;
            }
        }
    }

    Handles handles;
    _hoveredHandle = -1;

    if (handleLayer) {
        handles = handlesOf(*handleLayer, toFrame);

        if (_drag == Drag::None && hovered) {
            _hoveredHandle = handleAt(handles, point, toFrame);
        }
    }

    _cropKey = io.KeyAlt && handleLayer && handleLayer->halfWidth > 0.f;

    if (_hoveredHandle >= 0) {
        if (_hoveredHandle == 4) {
            rotationCursor();
        }
        else {
            const ImVec2 at = (_hoveredHandle < 4) ? handleLayer->corners[(size_t)_hoveredHandle] : handles.edges[_hoveredHandle - 5];
            ImGui::SetMouseCursor(resizeCursor(ImVec2(at.x - handles.center.x, at.y - handles.center.y)));
        }
    }

    // The layers under the mouse, the top one first.
    std::vector<const Layer*> underMouse;

    if (hovered) {
        for (const Layer& layer : layers) {
            if (inside(layer.corners, point)) {
                underMouse.push_back(&layer);
            }
        }
    }

    if (hovered && _hoveredHandle < 0 && _drag == Drag::None && !underMouse.empty() && underMouse.front()->movable) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    auto startDrag = [&](const Layer& layer, Drag drag) {
        _drag = drag;
        _dragged = layer.id;
        _start = layer;
        _startMouse = point;
        _center = ImVec2(0.5f * frameWidth + layer.x, 0.5f * frameHeight + layer.y);
        _moved = false;
        _guides.clear();
        result.dragStarted = true;
    };

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && _drag == Drag::None) {
        if (_hoveredHandle >= 0) {
            startDrag(*handleLayer, (_hoveredHandle == 4) ? Drag::Rotate : (_hoveredHandle >= 5 || _cropKey) ? Drag::Crop : Drag::Scale);

            // The sides of the handle: corners top left, top right, bottom
            // right, bottom left; edges top, right, bottom, left.
            const int h = _hoveredHandle;
            _cropLeft = h == 0 || h == 3 || h == 8;
            _cropRight = h == 1 || h == 2 || h == 6;
            _cropTop = h == 0 || h == 1 || h == 5;
            _cropBottom = h == 2 || h == 3 || h == 7;

            const ImVec2 at = (h < 4) ? handleLayer->corners[(size_t)h] : (h == 4) ? handles.rotation : handles.edges[h - 5];
            _cursor = (h == 4) ? ImGuiMouseCursor_None : resizeCursor(ImVec2(at.x - handles.center.x, at.y - handles.center.y));
            _center = handles.center;
        }
        else {
            // The top layer under the mouse. Ctrl: the one below the
            // selected layer there, to reach a layer hidden by another
            // (again and again to go down, around to the top).
            const Layer* picked = underMouse.empty() ? nullptr : underMouse.front();

            if (io.KeyCtrl && underMouse.size() > 1) {
                for (size_t i = 0; i < underMouse.size(); i++) {
                    if (std::find(selection.begin(), selection.end(), underMouse[i]->id) != selection.end()) {
                        picked = underMouse[(i + 1) % underMouse.size()];
                        break;
                    }
                }
            }

            if (picked) {
                result.clicked = picked->id;
                result.doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

                if (picked->movable) {
                    startDrag(*picked, Drag::Move);
                }
            }
            else {
                result.clickedEmpty = true;
            }
        }
    }

    if (_drag == Drag::None) {
        return result;
    }

    // The dragged layer is still there?
    const auto current = std::find_if(layers.begin(), layers.end(), [&](const Layer& layer) { return layer.id == _dragged; });

    if (current == layers.end() || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        result.finished = _moved && current != layers.end();
        result.drag = _drag;
        result.layer = (current != layers.end()) ? *current : _start;
        _drag = Drag::None;
        _guides.clear();
        return result;
    }

    Layer layer = _start;
    const float snap = SnapDistance * toFrame;
    _guides.clear();

    if (_drag == Drag::Move) {
        const float dx = point.x - _startMouse.x;
        const float dy = point.y - _startMouse.y;

        if (std::abs(dx) > 1.f || std::abs(dy) > 1.f || _moved) {
            float x = _start.x + dx;
            float y = _start.y + dy;

            // Its edges and middle snap to the edges and middle of the
            // frame and of the other layers. Alt: moves freely.
            if (!io.KeyAlt) {
                ImVec2 min, max;
                boundsOf(_start.corners, min, max);
                const ImVec2 center(0.5f * frameWidth + x, 0.5f * frameHeight + y);
                const float startCenterX = 0.5f * frameWidth + _start.x;
                const float startCenterY = 0.5f * frameHeight + _start.y;

                // Its edges and middle, from its center.
                const float ownX[3] = { min.x - startCenterX, 0.5f * (min.x + max.x) - startCenterX, max.x - startCenterX };
                const float ownY[3] = { min.y - startCenterY, 0.5f * (min.y + max.y) - startCenterY, max.y - startCenterY };

                std::vector<float> targetsX = { 0.f, 0.5f * frameWidth, frameWidth };
                std::vector<float> targetsY = { 0.f, 0.5f * frameHeight, frameHeight };

                for (const Layer& other : layers) {
                    if (other.id == _dragged) {
                        continue;
                    }

                    ImVec2 otherMin, otherMax;
                    boundsOf(other.corners, otherMin, otherMax);
                    targetsX.insert(targetsX.end(), { otherMin.x, 0.5f * (otherMin.x + otherMax.x), otherMax.x });
                    targetsY.insert(targetsY.end(), { otherMin.y, 0.5f * (otherMin.y + otherMax.y), otherMax.y });
                }

                auto snapAxis = [&](float c, const float own[3], const std::vector<float>& targets, float& shift, float& at) {
                    float best = snap;
                    bool found = false;

                    for (int i = 0; i < 3; i++) {
                        for (float target : targets) {
                            const float distance = std::abs(c + own[i] - target);

                            if (distance < best) {
                                best = distance;
                                shift = target - (c + own[i]);
                                at = target;
                                found = true;
                            }
                        }
                    }

                    return found;
                };

                float shift = 0.f;
                float at = 0.f;

                if (snapAxis(center.x, ownX, targetsX, shift, at)) {
                    x += shift;
                    _guides.push_back({ true, at });
                }

                if (snapAxis(center.y, ownY, targetsY, shift, at)) {
                    y += shift;
                    _guides.push_back({ false, at });
                }
            }

            layer.x = std::round(x);
            layer.y = std::round(y);
            _moved = true;
        }

        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    else if (_drag == Drag::Scale) {
        // Uniform scale around the center: the distance to the center,
        // relative to the one when the drag started.
        const float startDistance = std::max(1.f, std::hypot(_startMouse.x - _center.x, _startMouse.y - _center.y));
        const float distance = std::hypot(point.x - _center.x, point.y - _center.y);
        float scale = std::max(0.01f, _start.scale * distance / startDistance);

        // Snaps to 100 %, else an edge to an edge of the frame. Ctrl:
        // scales freely.
        if (!io.KeyCtrl) {
            if (std::abs(scale - 1.f) < 0.03f) {
                scale = 1.f;
            }
            else {
                ImVec2 min, max;
                boundsOf(_start.corners, min, max);
                const float centerX = 0.5f * frameWidth + _start.x;
                const float centerY = 0.5f * frameHeight + _start.y;
                const float ratio = scale / std::max(1e-3f, _start.scale);
                float best = snap;
                float snapped = scale;
                Guide guide;
                bool found = false;

                // An edge at `offset` from the center (at the start scale),
                // onto `target`.
                auto tryEdge = [&](float center, float offset, float target, bool vertical) {
                    if (std::abs(offset) < 1.f) {
                        return;
                    }

                    const float distance = std::abs(center + offset * ratio - target);
                    const float wanted = _start.scale * (target - center) / offset;

                    if (distance < best && wanted > 0.01f) {
                        best = distance;
                        snapped = wanted;
                        guide = { vertical, target };
                        found = true;
                    }
                };

                for (float target : { 0.f, frameWidth }) {
                    tryEdge(centerX, min.x - centerX, target, true);
                    tryEdge(centerX, max.x - centerX, target, true);
                }

                for (float target : { 0.f, frameHeight }) {
                    tryEdge(centerY, min.y - centerY, target, false);
                    tryEdge(centerY, max.y - centerY, target, false);
                }

                if (found) {
                    scale = snapped;
                    _guides.push_back(guide);
                }
            }
        }

        layer.scale = scale;
        _moved = true;
        ImGui::SetMouseCursor(_cursor);
    }
    else if (_drag == Drag::Crop) {
        // The side follows the mouse, in the picture; the rest of it stays
        // in place. Snaps to the edges and the middle, Ctrl to crop freely.
        if (_start.halfWidth > 0.f && _start.halfHeight > 0.f) {
            const ImVec2 local = toLayer(_start, frameWidth, frameHeight, point);
            const float width = 2.f * _start.halfWidth;
            const float height = 2.f * _start.halfHeight;
            const float cropSnap = snap / std::max(1e-3f, _start.scale);

            auto snapped = [&](float fraction, float size) {
                if (!io.KeyCtrl) {
                    for (float to : { 0.f, 0.5f }) {
                        if (std::abs(fraction - to) * size < cropSnap) return to;
                    }
                }

                return fraction;
            };

            if (_cropLeft) {
                layer.cropLeft = std::clamp(snapped((local.x + _start.halfWidth) / width, width), 0.f, std::max(0.f, 0.99f - layer.cropRight));
            }

            if (_cropRight) {
                layer.cropRight = std::clamp(snapped((_start.halfWidth - local.x) / width, width), 0.f, std::max(0.f, 0.99f - layer.cropLeft));
            }

            if (_cropTop) {
                layer.cropTop = std::clamp(snapped((local.y + _start.halfHeight) / height, height), 0.f, std::max(0.f, 0.99f - layer.cropBottom));
            }

            if (_cropBottom) {
                layer.cropBottom = std::clamp(snapped((_start.halfHeight - local.y) / height, height), 0.f, std::max(0.f, 0.99f - layer.cropTop));
            }

            _moved = true;
        }

        ImGui::SetMouseCursor(_cursor);
    }
    else if (_drag == Drag::Rotate) {
        const float startAngle = std::atan2(_startMouse.y - _center.y, _startMouse.x - _center.x);
        const float angle = std::atan2(point.y - _center.y, point.x - _center.x);
        float rotation = _start.rotation + (angle - startAngle) * 180.f / Pi;

        // Shift: steps of 15 degrees; else snaps to right angles (Ctrl:
        // freely).
        if (io.KeyShift) {
            rotation = std::round(rotation / 15.f) * 15.f;
        }
        else if (!io.KeyCtrl && std::abs(rotation - std::round(rotation / 90.f) * 90.f) < 3.f) {
            rotation = std::round(rotation / 90.f) * 90.f;
        }

        layer.rotation = rotation;
        _moved = true;
        rotationCursor();
    }

    if (_moved) {
        result.edited = true;
        result.layer = layer;
        result.drag = _drag;
    }

    return result;
}


void LayerCanvas::draw(ImDrawList* drawList, const ImVec2& frameMin, const ImVec2& frameSize, float frameWidth,
                       const std::vector<Layer>& layers, const std::vector<core::Id>& selection) const
{
    const float toFrame = frameWidth / std::max(1.f, frameSize.x);
    const float frameHeight = frameSize.y * toFrame;
    const ImVec2 frameMax(frameMin.x + frameSize.x, frameMin.y + frameSize.y);

    auto toScreen = [&](const ImVec2& p) {
        return ImVec2(frameMin.x + p.x / toFrame, frameMin.y + p.y / toFrame);
    };

    drawList->PushClipRect(frameMin, frameMax, true);

    // Guides of the snapping.
    for (const Guide& guide : _guides) {
        if (guide.vertical) {
            const float x = frameMin.x + guide.at / toFrame;
            drawList->AddLine(ImVec2(x, frameMin.y), ImVec2(x, frameMax.y), GuideColor, 1.f);
        }
        else {
            const float y = frameMin.y + guide.at / toFrame;
            drawList->AddLine(ImVec2(frameMin.x, y), ImVec2(frameMax.x, y), GuideColor, 1.f);
        }
    }

    const Layer* handleLayer = nullptr;

    for (const Layer& layer : layers) {
        if (std::find(selection.begin(), selection.end(), layer.id) == selection.end()) {
            continue;
        }

        ImVec2 corners[4];

        for (int i = 0; i < 4; i++) {
            corners[i] = toScreen(layer.corners[(size_t)i]);
        }

        drawList->AddPolyline(corners, 4, SelectedColor, ImDrawFlags_Closed, 1.5f);

        if (!selection.empty() && layer.id == selection.front() && layer.movable) {
            handleLayer = &layer;
        }
    }

    if (!handleLayer) {
        drawList->PopClipRect();
        return;
    }

    const Layer& layer = *handleLayer;
    const Handles handles = handlesOf(layer, toFrame);
    const bool croppable = layer.halfWidth > 0.f && layer.halfHeight > 0.f;

    // While cropping: the whole picture, faintly.
    const bool cropping = _drag == Drag::Crop && _dragged == layer.id;

    if (croppable && (cropping || _cropKey)) {
        ImVec2 whole[4];
        const ImVec2 local[4] = {
            { -layer.halfWidth, -layer.halfHeight }, { layer.halfWidth, -layer.halfHeight },
            { layer.halfWidth, layer.halfHeight }, { -layer.halfWidth, layer.halfHeight } };

        for (int i = 0; i < 4; i++) {
            whole[i] = toScreen(fromLayer(layer, frameWidth, frameHeight, local[i]));
        }

        drawList->AddPolyline(whole, 4, IM_COL32(120, 200, 255, 150), ImDrawFlags_Closed, 1.f);
    }

    // Handles: crop ones in blue (edges, and the corners with Alt).
    const float size = HandleRadius;

    for (int i = 0; i < 4; i++) {
        const ImVec2 c = toScreen(layer.corners[(size_t)i]);
        const ImU32 outline = _cropKey ? CropColor : SelectedColor;
        const ImU32 fill = (_hoveredHandle == i) ? outline : IM_COL32(255, 255, 255, 255);
        drawList->AddRectFilled(ImVec2(c.x - size, c.y - size), ImVec2(c.x + size, c.y + size), fill);
        drawList->AddRect(ImVec2(c.x - size, c.y - size), ImVec2(c.x + size, c.y + size), outline);
    }

    if (croppable) {
        const float edge = 0.75f * size;

        for (int i = 0; i < 4; i++) {
            const ImVec2 e = toScreen(handles.edges[i]);
            const ImU32 fill = (_hoveredHandle == 5 + i) ? CropColor : IM_COL32(255, 255, 255, 255);
            drawList->AddRectFilled(ImVec2(e.x - edge, e.y - edge), ImVec2(e.x + edge, e.y + edge), fill);
            drawList->AddRect(ImVec2(e.x - edge, e.y - edge), ImVec2(e.x + edge, e.y + edge), CropColor);
        }
    }

    if (layer.rotatable) {
        const ImVec2 rotation = toScreen(handles.rotation);
        const ImVec2 top = toScreen(handles.edges[0]);

        drawList->AddLine(top, rotation, SelectedColor, 1.5f);
        drawList->AddCircleFilled(rotation, size * 1.1f, (_hoveredHandle == 4) ? SelectedColor : IM_COL32(255, 255, 255, 255));
        drawList->AddCircle(rotation, size * 1.1f, SelectedColor, 0, 1.5f);
    }

    drawList->PopClipRect();
}
