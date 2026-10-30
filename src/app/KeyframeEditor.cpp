#include <app/KeyframeEditor.h>
#include <app/I18n.h>

#include <fonts/IconsMaterialDesign.h>

#include <algorithm>
#include <cmath>
#include <cstdio>


using namespace core;


namespace {

constexpr ImU32 CurveColor = IM_COL32(255, 170, 60, 255);
constexpr ImU32 KeyColor = IM_COL32(255, 255, 255, 255);
constexpr ImU32 SelectedKeyColor = IM_COL32(255, 122, 0, 255);
constexpr ImU32 PlayheadColor = IM_COL32(255, 122, 0, 200);

const char* easingName(Easing easing)
{
    switch (easing) {
        case Easing::Linear:    return tr("Linear");
        case Easing::EaseIn:    return tr("Ease In");
        case Easing::EaseOut:   return tr("Ease Out");
        case Easing::EaseInOut: return tr("Ease In and Out");
        case Easing::Hold:      return tr("Hold");
        default:                return "";
    }
}


void diamond(ImDrawList* drawList, const ImVec2& center, float size, ImU32 fill, ImU32 outline)
{
    const ImVec2 points[4] = {
        ImVec2(center.x, center.y - size), ImVec2(center.x + size, center.y),
        ImVec2(center.x, center.y + size), ImVec2(center.x - size, center.y) };

    drawList->AddConvexPolyFilled(points, 4, fill);
    drawList->AddPolyline(points, 4, outline, ImDrawFlags_Closed, 1.f);
}

} // namespace


std::vector<KeyframeEditor::Property> KeyframeEditor::propertiesOf(Project& project, Clip& clip)
{
    std::vector<Property> properties;

    Transform& t = clip.transform;
    properties.push_back({ "Transform", "Position X", &t.x });
    properties.push_back({ "Transform", "Position Y", &t.y });
    properties.push_back({ "Transform", "Scale", &t.scale, 0.f, 100.f, 100.f });
    properties.push_back({ "Transform", "Rotation", &t.rotation });
    properties.push_back({ "Transform", "Opacity", &t.opacity, 0.f, 1.f, 100.f });

    if (clip.kind == ClipKind::Generator) {
        if (const ShaderDef* shader = project.findShader(clip.shader)) {
            for (size_t i = 0; i < shader->parameters.size() && i < clip.shaderParameters.size(); i++) {
                const ShaderParameter& parameter = shader->parameters[i];
                properties.push_back({ shader->name, parameter.name, &clip.shaderParameters[i], parameter.minimum, parameter.maximum });
            }
        }
    }

    for (VideoEffect& effect : clip.videoEffects) {
        const VideoEffectInfo* info = effect.info();

        for (size_t i = 0; info && i < info->parameters.size() && i < effect.parameters.size(); i++) {
            const VideoEffectParameter& parameter = info->parameters[i];
            properties.push_back({ info->name, parameter.name, &effect.parameters[i], parameter.minimum, parameter.maximum, parameter.displayScale });
        }
    }

    return properties;
}


KeyframeEditor::Result KeyframeEditor::draw(Project& project, const std::vector<Id>& selection, Time& playhead)
{
    Result result;
    Clip* clip = selection.empty() ? nullptr : project.findClip(selection.front());
    const Track* track = clip ? project.findTrackOfClip(clip->id) : nullptr;

    if (!clip || !track || track->type != TrackType::Video) {
        ImGui::TextDisabled(tr("Select a video, image, text or generator clip to edit the curves of its animations."));
        return result;
    }

    if (clip->id != _clip) {
        _clip = clip->id;
        _selectedKeys.clear();
        _draggedKey = -1;
        _viewStart = _viewEnd = 0;
    }

    std::vector<Property> properties = propertiesOf(project, *clip);

    // The chosen property: the first animated one by default.
    Property* chosen = nullptr;

    for (Property& property : properties) {
        if (property.group + "/" + property.name == _property) chosen = &property;
    }

    if (!chosen) {
        for (Property& property : properties) {
            if (property.value->animated()) {
                chosen = &property;
                break;
            }
        }
    }

    if (!chosen) {
        chosen = &properties.front();
    }

    _property = chosen->group + "/" + chosen->name;

    // Properties
    const float listWidth = 13.f * ImGui::GetFontSize();

    if (ImGui::BeginChild("##properties", ImVec2(listWidth, 0.f), ImGuiChildFlags_Borders)) {
        std::string group;

        for (Property& property : properties) {
            if (property.group != group) {
                group = property.group;
                ImGui::SeparatorText(group.c_str());
            }

            ImGui::PushID(property.value);

            const bool animated = property.value->animated();
            const std::string label = std::string(animated ? ICON_MD_LENS " " : "    ") + tr(property.name) +
                (animated ? "  (" + std::to_string(property.value->keys.size()) + ")" : "");

            if (!animated) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }

            if (ImGui::Selectable(label.c_str(), &property == chosen)) {
                _property = property.group + "/" + property.name;
                _selectedKeys.clear();
            }

            if (!animated) {
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
        }
    }

    ImGui::EndChild();

    // The chosen one may have changed: look it up again.
    for (Property& property : properties) {
        if (property.group + "/" + property.name == _property) chosen = &property;
    }

    ImGui::SameLine();
    drawCurve(*clip, *chosen, project.sequence(), playhead, result);

    return result;
}


void KeyframeEditor::drawCurve(const Clip& clip, Property& property, const Sequence& sequence, Time& playhead, Result& result)
{
    AnimatedValue& value = *property.value;
    std::vector<Keyframe>& keys = value.keys;
    const ImGuiIO& io = ImGui::GetIO();
    const float unit = ImGui::GetFontSize();
    const Time frame = frameToTime(1, sequence.frameRate);

    // Tools
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted((tr(property.group) + " > " + tr(property.name)).c_str());
    ImGui::SameLine();

    const Time local = std::clamp<Time>(playhead - clip.start, 0, clip.duration);
    const bool atKey = value.keyAt(local) >= 0;

    if (ImGui::SmallButton(atKey ? tr(ICON_MD_REMOVE " Keyframe") : tr(ICON_MD_ADD " Keyframe"))) {
        if (atKey) {
            value.removeKey(local);
            result.edit = "Remove keyframe";
        }
        else {
            value.addKey(local);
            result.edit = "Add keyframe";
        }

        _selectedKeys.clear();
    }

    ImGui::SetItemTooltip(tr("At the playhead"));
    ImGui::SameLine();

    if (ImGui::SmallButton(tr(ICON_MD_FIT_SCREEN " Fit"))) {
        _viewStart = _viewEnd = 0;
    }

    ImGui::SameLine();
    ImGui::TextDisabled(tr("Double click: add   Drag: move (Shift: value only, Alt: time only)   Right click: easing   Del: remove"));

    // The area of the curve.
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 50.f), std::max(avail.y, 50.f));
    const ImVec2 max(min.x + size.x, min.y + size.y);

    ImGui::InvisibleButton("##curve", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused() || ImGui::IsWindowFocused();

    // Time: the clip, or a part of it (wheel to zoom, middle button to move).
    if (_viewEnd <= _viewStart) {
        _viewStart = 0;
        _viewEnd = std::max<Time>(clip.duration, frame);
    }

    const float left = min.x + unit;
    const float right = max.x - unit;
    const float top = min.y + 1.6f * unit;           // Room for the ruler
    const float bottom = max.y - unit;

    auto timeToX = [&](Time t) { return left + (right - left) * (float)((double)(t - _viewStart) / (double)(_viewEnd - _viewStart)); };
    auto xToTime = [&](float x) { return _viewStart + (Time)((double)(x - left) / (double)(right - left) * (double)(_viewEnd - _viewStart)); };

    if (hovered && io.MouseWheel != 0.f) {
        const Time center = xToTime(io.MousePos.x);
        const double factor = (io.MouseWheel > 0.f) ? 0.8 : 1.25;
        const Time start = center - (Time)((double)(center - _viewStart) * factor);
        const Time end = center + (Time)((double)(_viewEnd - center) * factor);

        if (end - start > 4 * frame) {
            _viewStart = start;
            _viewEnd = end;
        }
    }

    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f)) {
        const Time shift = (Time)((double)io.MouseDelta.x / (double)(right - left) * (double)(_viewEnd - _viewStart));
        _viewStart -= shift;
        _viewEnd -= shift;
    }

    // Values: those of the keys and of the curve, with a margin; frozen while
    // dragging so that the curve does not move under the mouse.
    float low = value.at(0), high = low;

    for (const Keyframe& key : keys) {
        low = std::min(low, key.value);
        high = std::max(high, key.value);
    }

    if (high - low < 1e-4f) {
        const float spread = std::max(std::abs(high) * 0.5f, (property.maximum - property.minimum) < 1e5f ? 0.1f * (property.maximum - property.minimum) : 1.f);
        low -= spread;
        high += spread;
    }

    const float margin = 0.12f * (high - low);
    low -= margin;
    high += margin;

    if (_draggedKey >= 0) {
        low = _dragMinimum;
        high = _dragMaximum;
    }

    auto valueToY = [&](float v) { return bottom - (bottom - top) * (v - low) / (high - low); };
    auto yToValue = [&](float y) { return low + (bottom - y) / (bottom - top) * (high - low); };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(min, max, true);
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.f);

    // The clip, and the graduations of values.
    drawList->AddRectFilled(ImVec2(timeToX(0), top), ImVec2(timeToX(clip.duration), bottom), IM_COL32(255, 255, 255, 10));

    for (int i = 0; i <= 4; i++) {
        const float v = low + (high - low) * (float)i / 4.f;
        const float y = valueToY(v);
        char text[32];
        std::snprintf(text, sizeof(text), "%.3g", v * property.displayScale);

        drawList->AddLine(ImVec2(left, y), ImVec2(right, y), IM_COL32(255, 255, 255, 25));
        drawList->AddText(ImVec2(left + 2.f, y - unit), IM_COL32(255, 255, 255, 90), text);
    }

    // Ruler: clicks move the playhead.
    drawList->AddRectFilled(min, ImVec2(max.x, top - 0.3f * unit), IM_COL32(0, 0, 0, 60));

    const double seconds = timeToSeconds(_viewEnd - _viewStart);
    const double step = seconds > 60 ? 10.0 : seconds > 20 ? 5.0 : seconds > 8 ? 1.0 : seconds > 2 ? 0.5 : 0.1;

    for (double s = std::ceil(timeToSeconds(_viewStart) / step) * step; s <= timeToSeconds(_viewEnd); s += step) {
        const float x = timeToX(secondsToTime(s));
        char text[32];
        std::snprintf(text, sizeof(text), "%.1fs", s);

        drawList->AddLine(ImVec2(x, top - 0.6f * unit), ImVec2(x, bottom), IM_COL32(255, 255, 255, 20));
        drawList->AddText(ImVec2(x + 2.f, min.y + 0.1f * unit), IM_COL32(255, 255, 255, 120), text);
    }

    const bool inRuler = io.MousePos.y < top - 0.3f * unit;

    if (hovered && inRuler && ImGui::IsMouseDown(ImGuiMouseButton_Left) && _draggedKey < 0) {
        playhead = clip.start + std::clamp<Time>(snapToFrame(xToTime(io.MousePos.x), sequence.frameRate), 0, clip.duration - 1);
        result.seeked = true;
    }

    // The curve, sampled every two pixels.
    std::vector<ImVec2> points;

    for (float x = left; x <= right; x += 2.f) {
        points.emplace_back(x, valueToY(value.at(xToTime(x))));
    }

    drawList->AddPolyline(points.data(), (int)points.size(), value.animated() ? CurveColor : IM_COL32(255, 170, 60, 110), 0, 2.f);

    // Keys: hovered, clicked, dragged.
    const float keySize = 0.42f * unit;
    int hoveredKey = -1;

    for (int i = 0; i < (int)keys.size(); i++) {
        const ImVec2 p(timeToX(keys[i].time), valueToY(keys[i].value));

        if (std::abs(io.MousePos.x - p.x) <= keySize * 1.6f && std::abs(io.MousePos.y - p.y) <= keySize * 1.6f) {
            hoveredKey = i;
        }
    }

    if (hovered && !inRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoveredKey >= 0) {
            if (io.KeyCtrl) {
                if (!_selectedKeys.erase(hoveredKey)) _selectedKeys.insert(hoveredKey);
            }
            else if (!_selectedKeys.count(hoveredKey)) {
                _selectedKeys = { hoveredKey };
            }

            _draggedKey = hoveredKey;
            _dragMoved = false;
            _dragStart = io.MousePos;
            _dragStartKeys = keys;
            _dragMinimum = low;
            _dragMaximum = high;
        }
        else if (!io.KeyCtrl) {
            _selectedKeys.clear();
        }
    }

    // Double click: a key on the curve there.
    if (hovered && !inRuler && hoveredKey < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const Time t = std::clamp<Time>(snapToFrame(xToTime(io.MousePos.x), sequence.frameRate), 0, clip.duration - 1);
        value.addKey(t);
        _selectedKeys = { value.keyAt(t) };
        result.edit = "Add keyframe";
    }

    if (_draggedKey >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (_dragMoved) {
                result.edit = "Move keyframe";
            }

            _draggedKey = -1;
        }
        else if (std::abs(io.MousePos.x - _dragStart.x) > 2.f || std::abs(io.MousePos.y - _dragStart.y) > 2.f || _dragMoved) {
            _dragMoved = true;

            // The selected keys follow, in order: between their neighbours.
            const Time dt = io.KeyShift ? 0 : snapToFrame(xToTime(io.MousePos.x), sequence.frameRate) - snapToFrame(xToTime(_dragStart.x), sequence.frameRate);
            const float dv = io.KeyAlt ? 0.f : yToValue(io.MousePos.y) - yToValue(_dragStart.y);

            for (int i : _selectedKeys) {
                if (i < 0 || i >= (int)keys.size() || i >= (int)_dragStartKeys.size()) continue;

                const Time lowest = (i > 0) ? _dragStartKeys[i - 1].time + (_selectedKeys.count(i - 1) ? dt : 0) + frame : 0;
                const Time highest = (i + 1 < (int)keys.size()) ? _dragStartKeys[i + 1].time + (_selectedKeys.count(i + 1) ? dt : 0) - frame : clip.duration - 1;

                keys[i].time = std::clamp<Time>(_dragStartKeys[i].time + dt, std::min(lowest, highest), std::max(lowest, highest));
                keys[i].value = std::clamp(_dragStartKeys[i].value + dv, property.minimum, property.maximum);
            }

            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
    }

    // Right click: the easing of the segment from the key, or removing it.
    static int contextKey = -1;

    if (hovered && hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        contextKey = hoveredKey;

        if (!_selectedKeys.count(hoveredKey)) {
            _selectedKeys = { hoveredKey };
        }

        ImGui::OpenPopup("##keyContext");
    }

    if (ImGui::BeginPopup("##keyContext")) {
        for (Easing easing : { Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut, Easing::Hold }) {
            const bool current = contextKey >= 0 && contextKey < (int)keys.size() && keys[contextKey].easing == easing;

            if (ImGui::MenuItem(easingName(easing), nullptr, current)) {
                for (int i : _selectedKeys) {
                    if (i >= 0 && i < (int)keys.size()) keys[i].easing = easing;
                }

                result.edit = "Change easing";
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem(tr(ICON_MD_DELETE " Remove"), "Del")) {
            _selectedKeys.insert(contextKey);
            const std::vector<int> removed(_selectedKeys.rbegin(), _selectedKeys.rend());

            for (int i : removed) {
                if (i >= 0 && i < (int)keys.size()) keys.erase(keys.begin() + i);
            }

            // The value stays where the curve was, once without keys.
            if (keys.empty()) value.value = value.at(local);

            _selectedKeys.clear();
            result.edit = "Remove keyframe";
        }

        ImGui::EndPopup();
    }

    // Delete: the selected keys.
    if (focused && !_selectedKeys.empty() && !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        const float kept = value.at(local);
        const std::vector<int> removed(_selectedKeys.rbegin(), _selectedKeys.rend());

        for (int i : removed) {
            if (i >= 0 && i < (int)keys.size()) keys.erase(keys.begin() + i);
        }

        if (keys.empty()) value.value = kept;

        _selectedKeys.clear();
        result.edit = "Remove keyframe";
    }

    // Keys drawn over the curve.
    for (int i = 0; i < (int)keys.size(); i++) {
        const ImVec2 p(timeToX(keys[i].time), valueToY(keys[i].value));
        const bool selected = _selectedKeys.count(i) > 0;
        diamond(drawList, p, keySize * (i == hoveredKey ? 1.2f : 1.f), selected ? SelectedKeyColor : KeyColor, IM_COL32(0, 0, 0, 200));
    }

    // Tooltip of the hovered key.
    if (hoveredKey >= 0 && hoveredKey < (int)keys.size() && _draggedKey < 0) {
        ImGui::SetTooltip("%.2f s: %.3g  (%s)", timeToSeconds(keys[hoveredKey].time), keys[hoveredKey].value * property.displayScale,
            easingName(keys[hoveredKey].easing));
    }

    // Playhead
    const float playheadX = timeToX(playhead - clip.start);
    drawList->AddLine(ImVec2(playheadX, min.y), ImVec2(playheadX, max.y), PlayheadColor, 1.5f);

    drawList->PopClipRect();
    ImGui::EndGroup();
}
