#include <app/Inspector.h>
#include <app/Widgets.h>

#include <fonts/IconsMaterialDesign.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <core/Layers.h>
#include <core/TextEdit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>


using namespace core;


namespace {

struct FrameRatePreset
{
    const char* label;
    Rational rate;
};

constexpr FrameRatePreset FrameRatePresets[] = {
    { "23.976", { 24000, 1001 } },
    { "24",     { 24, 1 } },
    { "25",     { 25, 1 } },
    { "29.97",  { 30000, 1001 } },
    { "30",     { 30, 1 } },
    { "50",     { 50, 1 } },
    { "59.94",  { 60000, 1001 } },
    { "60",     { 60, 1 } },
};


struct ResolutionPreset
{
    const char* label;
    uint32_t width;
    uint32_t height;
};

constexpr ResolutionPreset ResolutionPresets[] = {
    { "1280 x 720 (HD)",            1280,  720 },
    { "1920 x 1080 (Full HD)",      1920, 1080 },
    { "2560 x 1440 (QHD)",          2560, 1440 },
    { "3840 x 2160 (4K UHD)",       3840, 2160 },
    { "1080 x 1920 (Vertical)",     1080, 1920 },
    { "1080 x 1080 (Square)",       1080, 1080 },
};

constexpr uint32_t SampleRatePresets[] = { 44100, 48000, 96000 };

const char* const LetterAnimationNames[] = { "None", "Typewriter", "Fade", "Rise", "Pop" };


// Sets a keyframe, creating it when needed.
void setKey(AnimatedValue& value, Time t, float v, Easing easing)
{
    value.addKey(t, easing);
    const int index = value.keyAt(t);
    value.keys[index].value = v;
    value.keys[index].easing = easing;
}


bool colorEdit(const char* label, Color& color)
{
    float rgba[4] = { color.r, color.g, color.b, color.a };

    if (ImGui::ColorEdit4(label, rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
        color = { rgba[0], rgba[1], rgba[2], rgba[3] };
        return true;
    }

    return false;
}


// Label column of the property tables.
void propertyLabel(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
}


// Records the edit when the last widget was released after a change.
void finishedEdit(std::string& edit, const char* label)
{
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        edit = label;
    }
}

} // namespace


std::string Inspector::draw(Project& project, const std::vector<Id>& selection, Time playhead, FontLibrary& fonts, const TextSelection& textSelection)
{
    std::string edit;
    Clip* clip = selection.empty() ? nullptr : project.findClip(selection.front());

    if (!clip) {
        sequenceSection(project, edit);
        ImGui::Spacing();
        ImGui::TextDisabled("Select a clip to edit its properties.");
        return edit;
    }

    const Track* track = project.findTrackOfClip(clip->id);
    const MediaItem* media = project.findMedia(clip->mediaId);

    // A line of subtitles: its text and timing, the style of them all.
    if (track && track->type == TrackType::Subtitle) {
        subtitleSection(project, *clip, edit);
        return edit;
    }
    const bool video = track && track->type == TrackType::Video;
    const Sequence& sequence = project.sequence();
    const Time localTime = std::clamp<Time>(playhead - clip->start, 0, clip->duration);

    const char* title = (clip->kind == ClipKind::Text) ? ICON_MD_TEXT_FIELDS " Text"
        : (clip->kind == ClipKind::Generator) ? ICON_MD_AUTO_AWESOME " Generator"
        : (clip->kind == ClipKind::Sequence) ? ICON_MD_VIDEO_LIBRARY " Nested Sequence"
        : clip->frozen ? ICON_MD_PAUSE_CIRCLE " Freeze Frame"
        : video ? ICON_MD_MOVIE " Video Clip" : ICON_MD_AUDIOTRACK " Audio Clip";

    ImGui::SeparatorText(title);

    if (ImGui::BeginTable("##clipInfo", 2)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        auto row = [](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };

        if (media) {
            row("Media", media->name());
        }

        row("Track", track ? track->name : "?");
        row("Start", formatTimecode(clip->start, sequence.frameRate));
        row("Duration", formatTimecode(clip->duration, sequence.frameRate));

        if (media) {
            row(clip->frozen ? "Frame" : "Source In", formatTimecode(clip->sourceIn, sequence.frameRate));
        }

        ImGui::EndTable();
    }

    if (media && clip->kind == ClipKind::Media && !clip->frozen && media->type != MediaType::Image) {
        speedSection(project, *clip, edit);
    }

    if (clip->kind == ClipKind::Text) {
        textSection(*clip, fonts, textSelection, edit);
    }

    if (clip->kind == ClipKind::Generator) {
        generatorSection(project, *clip, localTime, edit);
    }

    if (video) {
        transformSection(*clip, localTime, edit);

        if (clip->kind != ClipKind::Text) {
            frameSection(*clip, edit);
        }

        presetsSection(*clip, edit);
        transitionSection(project, *clip, edit);
    }
    else {
        ImGui::SeparatorText("Audio");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##gain", &clip->gainDb, -60.f, 12.f, "Gain %.1f dB");
        finishedEdit(edit, "Change gain");

        const float half = 0.5f * ImGui::GetContentRegionAvail().x - 0.5f * ImGui::GetStyle().ItemSpacing.x;
        const float maxFade = (float)timeToSeconds(clip->duration);

        float fadeIn = (float)timeToSeconds(clip->fadeIn);
        ImGui::SetNextItemWidth(half);
        if (ImGui::DragFloat("##fadeIn", &fadeIn, 0.01f, 0.f, maxFade, "Fade in %.2f s")) {
            clip->fadeIn = std::clamp<Time>(secondsToTime(fadeIn), 0, clip->duration);
        }
        finishedEdit(edit, "Change fade");

        ImGui::SameLine();

        float fadeOut = (float)timeToSeconds(clip->fadeOut);
        ImGui::SetNextItemWidth(half);
        if (ImGui::DragFloat("##fadeOut", &fadeOut, 0.01f, 0.f, maxFade, "Fade out %.2f s")) {
            clip->fadeOut = std::clamp<Time>(secondsToTime(fadeOut), 0, clip->duration);
        }
        finishedEdit(edit, "Change fade");

        ImGui::TextDisabled("Or drag the handles at the top corners of the clip.");
    }

    if (selection.size() > 1) {
        ImGui::Spacing();
        ImGui::TextDisabled("%zu clips selected", selection.size());
    }

    return edit;
}


bool Inspector::animatedRow(const char* label, AnimatedValue& value, Time localTime,
    float speed, float min, float max, const char* format, float displayScale, std::string& edit)
{
    propertyLabel(label);
    ImGui::PushID(label);

    // Keyframe button: filled at a keyframe, outlined when animated between
    // keyframes.
    const bool atKey = value.keyAt(localTime) >= 0;
    const char* icon = atKey ? ICON_MD_RADIO_BUTTON_CHECKED : (value.animated() ? ICON_MD_RADIO_BUTTON_UNCHECKED : ICON_MD_ADD_CIRCLE_OUTLINE);

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));

    if (value.animated()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 170, 60, 255));
    }

    const bool toggled = ImGui::Button(icon);

    if (value.animated()) {
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip(atKey ? "Remove the keyframe" : "Add a keyframe at the playhead");
    }

    if (toggled) {
        if (atKey) {
            value.removeKey(localTime);
            edit = "Remove keyframe";
        }
        else {
            value.addKey(localTime);
            edit = "Add keyframe";
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);

    float shown = value.at(localTime) * displayScale;
    bool changed = false;

    if (ImGui::DragFloat("##value", &shown, speed, min, max, format)) {
        value.set(localTime, shown / displayScale);
        changed = true;
    }

    finishedEdit(edit, value.animated() ? "Change keyframe" : "Change property");

    // Right click: more keyframe commands.
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Remove All Keyframes", nullptr, false, value.animated())) {
            value.clearKeys(localTime);
            edit = "Remove keyframes";
        }

        ImGui::EndPopup();
    }

    ImGui::PopID();

    return changed;
}


void Inspector::transformSection(Clip& clip, Time localTime, std::string& edit)
{
    ImGui::SeparatorText(ICON_MD_OPEN_WITH " Transform");

    if (!ImGui::BeginTable("##transform", 2)) {
        return;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

    Transform& t = clip.transform;

    // Pictures: their size in the frame before the scale. Native shows
    // them at their resolution when the scale is 100 %.
    if (clip.kind == ClipKind::Media || clip.kind == ClipKind::Sequence) {
        propertyLabel("Size");

        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float width = (ImGui::GetContentRegionAvail().x - 2.f * spacing) / 3.f;

        for (FitMode mode : { FitMode::Fit, FitMode::Fill, FitMode::Native }) {
            if (mode != FitMode::Fit) {
                ImGui::SameLine(0.f, spacing);
            }

            if (widgets::optionButton(fitModeName(mode), clip.fit == mode, ImVec2(width, 0.f))) {
                clip.fit = mode;
                edit = "Change size";
            }

            ImGui::SetItemTooltip("%s", mode == FitMode::Fit ? "The whole picture, as large as the frame allows"
                : mode == FitMode::Fill ? "The picture covers the whole frame, cut at the edges"
                : "One pixel of the picture per pixel of the sequence: 100 % is the native resolution");
        }
    }

    animatedRow("Position X", t.x, localTime, 1.f, -100000.f, 100000.f, "%.0f px", 1.f, edit);
    animatedRow("Position Y", t.y, localTime, 1.f, -100000.f, 100000.f, "%.0f px", 1.f, edit);
    animatedRow("Scale", t.scale, localTime, 0.5f, 0.f, 10000.f, "%.1f %%", 100.f, edit);
    animatedRow("Rotation", t.rotation, localTime, 0.5f, -3600.f, 3600.f, "%.1f\xc2\xb0", 1.f, edit);
    animatedRow("Opacity", t.opacity, localTime, 0.5f, 0.f, 100.f, "%.0f %%", 100.f, edit);

    ImGui::EndTable();

    if (ImGui::Button(ICON_MD_RESTART_ALT " Reset")) {
        t = Transform();
        edit = "Reset transform";
    }
}


void Inspector::frameSection(Clip& clip, std::string& edit)
{
    ImGui::SeparatorText(ICON_MD_CROP " Crop & Frame");

    Frame& frame = clip.frame;

    // The usual crops, for side by side comparisons.
    const float third = (ImGui::GetContentRegionAvail().x - 2.f * ImGui::GetStyle().ItemSpacing.x) / 3.f;

    if (ImGui::Button("Left Half", ImVec2(third, 0.f))) {
        frame.cropLeft = frame.cropTop = frame.cropBottom = 0.f;
        frame.cropRight = 0.5f;
        edit = "Crop";
    }

    ImGui::SetItemTooltip("Keep the left half of the picture, in place");
    ImGui::SameLine();

    if (ImGui::Button("Right Half", ImVec2(third, 0.f))) {
        frame.cropRight = frame.cropTop = frame.cropBottom = 0.f;
        frame.cropLeft = 0.5f;
        edit = "Crop";
    }

    ImGui::SetItemTooltip("Keep the right half of the picture, in place");
    ImGui::SameLine();

    if (ImGui::Button("No Crop", ImVec2(third, 0.f))) {
        frame.cropLeft = frame.cropRight = frame.cropTop = frame.cropBottom = 0.f;
        edit = "Crop";
    }

    if (!ImGui::BeginTable("##frame", 2)) {
        return;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

    // Percents of the picture; what is left of it never vanishes.
    auto cropRow = [&](const char* label, float& value, float opposite) {
        propertyLabel(label);
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(-FLT_MIN);

        float percent = value * 100.f;

        if (ImGui::SliderFloat("##crop", &percent, 0.f, 100.f, "%.1f %%")) {
            value = std::clamp(percent / 100.f, 0.f, std::max(0.f, 0.99f - opposite));
        }

        finishedEdit(edit, "Crop");
        ImGui::PopID();
    };

    cropRow("Left", frame.cropLeft, frame.cropRight);
    cropRow("Right", frame.cropRight, frame.cropLeft);
    cropRow("Top", frame.cropTop, frame.cropBottom);
    cropRow("Bottom", frame.cropBottom, frame.cropTop);

    propertyLabel("Corners");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##corners", &frame.cornerRadius, 0.5f, 0.f, 2000.f, "%.0f px");
    finishedEdit(edit, "Change corners");

    propertyLabel("Border");
    colorEdit("##borderColor", frame.borderColor);
    finishedEdit(edit, "Change border");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##border", &frame.borderWidth, 0.2f, 0.f, 500.f, "%.1f px");
    finishedEdit(edit, "Change border");

    propertyLabel("Shadow");
    colorEdit("##shadowColor", frame.shadowColor);
    finishedEdit(edit, "Change shadow");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    float offset[2] = { frame.shadowOffsetX, frame.shadowOffsetY };

    if (ImGui::DragFloat2("##shadowOffset", offset, 0.5f, -2000.f, 2000.f, "%.0f px")) {
        frame.shadowOffsetX = offset[0];
        frame.shadowOffsetY = offset[1];
    }

    finishedEdit(edit, "Change shadow");

    propertyLabel("Blur");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##shadowSoftness", &frame.shadowSoftness, 0.5f, 0.f, 500.f, "%.0f px");
    finishedEdit(edit, "Change shadow");

    ImGui::EndTable();

    if (ImGui::Button(ICON_MD_RESTART_ALT " Reset##frame")) {
        frame = Frame();
        edit = "Reset frame";
    }
}


void Inspector::speedSection(Project& project, Clip& clip, std::string& edit)
{
    ImGui::SeparatorText(ICON_MD_SPEED " Speed");

    // In percent; the linked clips (the sound of a video) follow.
    float percent = (float)(clip.speed * 100.0);
    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::DragFloat("##speed", &percent, 0.5f, (float)(Project::MinSpeed * 100.0), (float)(Project::MaxSpeed * 100.0),
            "%.0f %%", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        project.setClipSpeed(clip.id, (double)percent / 100.0);
    }

    ImGui::SetItemTooltip("Playback speed: the clip gets shorter or longer, the clips after it follow.\n"
        "The sound plays faster or slower (higher or lower).");
    finishedEdit(edit, "Change speed");

    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    constexpr double presets[] = { 0.25, 0.5, 1.0, 1.5, 2.0, 4.0 };
    const float width = (ImGui::GetContentRegionAvail().x - spacing * (float)(std::size(presets) - 1)) / (float)std::size(presets);

    for (size_t i = 0; i < std::size(presets); i++) {
        if (i > 0) {
            ImGui::SameLine(0.f, spacing);
        }

        const std::string label = std::to_string((int)(presets[i] * 100.0)) + "%";
        const bool current = std::abs(clip.speed - presets[i]) < 1e-6;

        if (widgets::optionButton(label.c_str(), current, ImVec2(width, 0.f)) && project.setClipSpeed(clip.id, presets[i])) {
            edit = "Change speed";
        }
    }
}


void Inspector::videoEffectsSection(Project& project, std::vector<VideoEffect>& effects, Time localTime, std::string& edit)
{
    int removed = -1;
    int moveUp = -1;

    for (int i = 0; i < (int)effects.size(); i++) {
        VideoEffect& effect = effects[i];
        const VideoEffectInfo* info = effect.info();

        if (!info) {
            continue;
        }

        ImGui::PushID((int)effect.id);

        if (ImGui::Checkbox("##enabled", &effect.enabled)) {
            edit = effect.enabled ? "Enable effect" : "Disable effect";
        }

        ImGui::SetItemTooltip("On / off");
        ImGui::SameLine();

        const std::string title = std::string(info->icon) + " " + info->name;
        const float buttons = 2.f * ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemAllowOverlap();
        const bool open = ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttons);
        ImGui::BeginDisabled(i == 0);

        if (ImGui::SmallButton(ICON_MD_ARROW_UPWARD)) {
            moveUp = i;
        }

        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Earlier in the chain");
        ImGui::SameLine();

        if (ImGui::SmallButton(ICON_MD_DELETE)) {
            removed = i;
        }

        ImGui::SetItemTooltip("Remove");

        if (open && ImGui::BeginTable("##parameters", 2)) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            size_t first = 0;

            // A color, from its three components.
            if (info->colorParameter && effect.parameters.size() >= 3) {
                propertyLabel("Color");
                float rgb[3] = { effect.parameters[0].at(localTime), effect.parameters[1].at(localTime), effect.parameters[2].at(localTime) };

                if (ImGui::ColorEdit3("##color", rgb, ImGuiColorEditFlags_NoInputs)) {
                    for (int c = 0; c < 3; c++) effect.parameters[c].set(localTime, rgb[c]);
                }

                finishedEdit(edit, "Change effect");
                ImGui::SameLine();
                ImGui::TextDisabled("Pick the key color with the dropper.");
                first = 3;
            }

            for (size_t p = first; p < info->parameters.size() && p < effect.parameters.size(); p++) {
                const VideoEffectParameter& parameter = info->parameters[p];
                const float range = (parameter.maximum - parameter.minimum) * parameter.displayScale;

                animatedRow(parameter.name, effect.parameters[p], localTime, range / 300.f,
                    parameter.minimum * parameter.displayScale, parameter.maximum * parameter.displayScale,
                    parameter.format, parameter.displayScale, edit);
            }

            ImGui::EndTable();
        }

        ImGui::PopID();
    }

    if (moveUp > 0) {
        std::swap(effects[moveUp], effects[moveUp - 1]);
        edit = "Reorder effects";
    }

    if (removed >= 0) {
        effects.erase(effects.begin() + removed);
        edit = "Remove effect";
    }

    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::BeginCombo("##addVideoEffect", ICON_MD_ADD " Add a video effect...", ImGuiComboFlags_HeightLarge)) {
        for (const VideoEffectInfo& info : videoEffectInfos()) {
            if (ImGui::Selectable((std::string(info.icon) + " " + info.name).c_str())) {
                effects.push_back(makeVideoEffect(project.newId(), info.id));
                edit = "Add effect";
            }
        }

        ImGui::EndCombo();
    }
}


void Inspector::presetsSection(Clip& clip, std::string& edit)
{
    ImGui::SeparatorText(ICON_MD_AUTO_AWESOME " Animation Presets");

    Transform& t = clip.transform;
    const Time duration = clip.duration;
    const Time half = std::min<Time>(TicksPerSecond / 2, duration / 2);

    // The presets start from the static values (kept while animated), so
    // that applying one again, or another one, does not compound.
    const float scale = t.scale.value;
    const float opacity = (t.opacity.value > 0.f) ? t.opacity.value : 1.f;

    const float width = 0.5f * (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x);

    if (ImGui::Button("Zoom In", ImVec2(width, 0))) {
        // Grows into place while appearing, then stays.
        const Time length = std::min<Time>(TicksPerSecond * 6 / 10, duration);
        t.scale.keys.clear();
        setKey(t.scale, 0, scale * 0.8f, Easing::EaseOut);
        setKey(t.scale, length, scale, Easing::EaseInOut);
        t.opacity.keys.clear();
        setKey(t.opacity, 0, 0.f, Easing::EaseOut);
        setKey(t.opacity, length * 2 / 3, opacity, Easing::EaseInOut);
        edit = "Zoom in preset";
    }

    ImGui::SetItemTooltip("Appears by growing into place, then stays.");

    ImGui::SameLine();

    if (ImGui::Button("Slow Zoom", ImVec2(width, 0))) {
        // Ken Burns effect: slow zoom into the picture over the whole clip.
        t.scale.keys.clear();
        setKey(t.scale, 0, scale, Easing::Linear);
        setKey(t.scale, duration, scale * 1.15f, Easing::Linear);
        edit = "Slow zoom preset";
    }

    ImGui::SetItemTooltip("Zooms slowly into the picture over the whole clip (Ken Burns).");

    if (ImGui::Button("Fade In", ImVec2(width, 0))) {
        const float end = t.opacity.at(half);
        std::erase_if(t.opacity.keys, [&](const Keyframe& k) { return k.time <= half; });
        setKey(t.opacity, 0, 0.f, Easing::EaseInOut);
        setKey(t.opacity, half, end > 0.f ? end : 1.f, Easing::EaseInOut);
        edit = "Fade in preset";
    }

    ImGui::SetItemTooltip("Fades in over the first half second.");

    ImGui::SameLine();

    if (ImGui::Button("Fade Out", ImVec2(width, 0))) {
        const float start = t.opacity.at(duration - half);
        std::erase_if(t.opacity.keys, [&](const Keyframe& k) { return k.time >= duration - half; });
        setKey(t.opacity, duration - half, start > 0.f ? start : 1.f, Easing::EaseInOut);
        setKey(t.opacity, duration, 0.f, Easing::EaseInOut);
        edit = "Fade out preset";
    }

    ImGui::SetItemTooltip("Fades out over the last half second.");

    if (ImGui::Button(ICON_MD_CLEAR " Remove Animation", ImVec2(-FLT_MIN, 0))) {
        for (AnimatedValue* value : { &t.x, &t.y, &t.scale, &t.rotation, &t.opacity }) {
            if (value->animated()) {
                value->clearKeys(value->keys.back().time);
            }
        }

        edit = "Remove animation";
    }
}


void Inspector::subtitleSection(Project& project, Clip& clip, std::string& edit)
{
    const Sequence& sequence = project.sequence();
    ImGui::SeparatorText(ICON_MD_SUBTITLES " Subtitle");

    // The text: one span, plain.
    if (clip.text.spans.empty()) {
        clip.text.spans.emplace_back();
    }

    std::string text = clip.text.plainText();

    if (ImGui::InputTextMultiline("##subtitle", &text, ImVec2(-FLT_MIN, 4.f * ImGui::GetTextLineHeightWithSpacing()))) {
        clip.text.spans.resize(1);
        clip.text.spans[0].text = text;
    }

    finishedEdit(edit, "Edit subtitle");

    if (ImGui::BeginTable("##subtitleTiming", 2)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        propertyLabel("Start");
        ImGui::TextUnformatted(formatTimecode(clip.start, sequence.frameRate).c_str());

        propertyLabel("Duration");
        ImGui::SetNextItemWidth(-FLT_MIN);
        float seconds = (float)timeToSeconds(clip.duration);

        if (ImGui::DragFloat("##duration", &seconds, 0.01f, 0.1f, 600.f, "%.2f s")) {
            Time freeStart, freeEnd;
            const Track* track = project.findTrackOfClip(clip.id);
            Time limit = secondsToTime(600.0);

            if (track && project.freeRangeAt(*track, clip.start, clip.id, freeStart, freeEnd)) {
                limit = freeEnd - clip.start;
            }

            clip.duration = std::clamp<Time>(secondsToTime(seconds), frameToTime(1, sequence.frameRate), limit);
        }

        finishedEdit(edit, "Change subtitle duration");
        ImGui::EndTable();
    }

    subtitleStyleSection(project, edit);
}


void Inspector::subtitleStyleSection(Project& project, std::string& edit)
{
    SubtitleStyle& style = project.sequence().subtitleStyle;
    ImGui::SeparatorText(ICON_MD_FORMAT_COLOR_TEXT " Subtitles of the Sequence");

    if (!ImGui::BeginTable("##subtitleStyle", 2)) {
        return;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

    propertyLabel("Size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##subtitleSize", &style.size, 0.5f, 8.f, 400.f, "%.0f px");
    finishedEdit(edit, "Change subtitles");

    propertyLabel("Color");
    colorEdit("##subtitleColor", style.color);
    finishedEdit(edit, "Change subtitles");

    propertyLabel("Outline");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##subtitleOutline", &style.outlineWidth, 0.1f, 0.f, 20.f, "%.1f px");
    finishedEdit(edit, "Change subtitles");

    propertyLabel("Margin");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat("##subtitleMargin", &style.bottomMargin, 0.5f, -2000.f, 2000.f, "%.0f px");
    ImGui::SetItemTooltip("From the bottom of the frame");
    finishedEdit(edit, "Change subtitles");

    ImGui::EndTable();
}


void Inspector::generatorSection(Project& project, Clip& clip, Time localTime, std::string& edit)
{
    const ShaderDef* shader = project.findShader(clip.shader);
    ImGui::SeparatorText((std::string(ICON_MD_AUTO_AWESOME " ") + (shader ? shader->name : clip.shader)).c_str());

    if (!shader) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " Shader not found");
        return;
    }

    // As many values as the shader has parameters.
    if (clip.shaderParameters.size() < shader->parameters.size()) {
        for (size_t i = clip.shaderParameters.size(); i < shader->parameters.size(); i++) {
            clip.shaderParameters.emplace_back(shader->parameters[i].defaultValue);
        }
    }

    if (!shader->parameters.empty() && ImGui::BeginTable("##shaderParameters", 2)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        for (size_t i = 0; i < shader->parameters.size(); i++) {
            const ShaderParameter& parameter = shader->parameters[i];
            animatedRow(parameter.name.c_str(), clip.shaderParameters[i], localTime, (parameter.maximum - parameter.minimum) / 300.f,
                parameter.minimum, parameter.maximum, "%.3g", 1.f, edit);
        }

        ImGui::EndTable();
    }

    const std::string error = shaderError ? shaderError(shader->id) : std::string();

    if (!error.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", error.c_str());
        ImGui::PopTextWrapPos();
    }

    if (shader->id.rfind("file:", 0) == 0 && editShader && ImGui::Button(ICON_MD_EDIT " Edit Shader")) {
        editShader(shader->id);
    }
}


void Inspector::transitionSection(Project& project, Clip& clip, std::string& edit)
{
    ImGui::SeparatorText(ICON_MD_COMPARE_ARROWS " Transition In");

    if (!ImGui::BeginTable("##transition", 2)) {
        return;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

    propertyLabel("Type");
    ImGui::SetNextItemWidth(-FLT_MIN);

    const ShaderDef* current = (clip.transitionIn.type == TransitionType::Shader) ? project.findShader(clip.transitionIn.shader) : nullptr;
    const std::string currentName = current ? std::string(ICON_MD_AUTO_AWESOME " ") + current->name : transitionName(clip.transitionIn.type);

    if (ImGui::BeginCombo("##type", currentName.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int type = 0; type <= (int)TransitionType::Zoom; type++) {
            const bool selected = (int)clip.transitionIn.type == type;

            if (ImGui::Selectable(transitionName((TransitionType)type), selected) && !selected) {
                clip.transitionIn.type = (TransitionType)type;
                edit = (type == 0) ? "Remove transition" : "Change transition";
            }
        }

        // Shaders: built in, then the user's.
        ImGui::Separator();

        auto shaderItem = [&](const ShaderDef& shader) {
            if (shader.kind != ShaderKind::Transition) {
                return;
            }

            const bool selected = current && current->id == shader.id;

            if (ImGui::Selectable((std::string(ICON_MD_AUTO_AWESOME " ") + shader.name).c_str(), selected) && !selected) {
                clip.transitionIn.type = TransitionType::Shader;
                clip.transitionIn.shader = shader.id;

                if (shader.id.rfind("builtin:", 0) != 0) {
                    project.storeShader(shader);
                }

                edit = "Change transition";
            }
        };

        for (const ShaderDef& shader : builtinShaders()) shaderItem(shader);

        if (userShaders) {
            for (const ShaderDef& shader : *userShaders) shaderItem(shader);
        }

        // A new one, from an example, opened in the editor.
        if (newShader) {
            ImGui::Separator();

            if (ImGui::Selectable(ICON_MD_ADD " New Transition Shader...")) {
                if (const ShaderDef* shader = newShader(ShaderKind::Transition)) {
                    clip.transitionIn.type = TransitionType::Shader;
                    clip.transitionIn.shader = shader->id;
                    project.storeShader(*shader);
                    edit = "Change transition";
                }
            }
        }

        ImGui::EndCombo();
    }

    if (current && current->id.rfind("file:", 0) == 0 && editShader) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();

        if (ImGui::Button(ICON_MD_EDIT " Edit Shader")) {
            editShader(current->id);
        }
    }

    if (current && shaderError) {
        const std::string error = shaderError(current->id);

        if (!error.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.f), ICON_MD_WARNING " %s", error.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    ImGui::BeginDisabled(clip.transitionIn.type == TransitionType::None);

    propertyLabel("Duration");
    ImGui::SetNextItemWidth(-FLT_MIN);
    float seconds = (float)timeToSeconds(clip.transitionIn.duration);

    if (ImGui::DragFloat("##duration", &seconds, 0.01f, 0.04f, 10.f, "%.2f s")) {
        clip.transitionIn.duration = secondsToTime(std::max(0.04f, seconds));
    }

    finishedEdit(edit, "Change transition duration");

    ImGui::EndDisabled();
    ImGui::EndTable();

    ImGui::TextDisabled("Centered on the start of the clip.");
}


bool Inspector::fontCombo(const char* label, std::string& font, FontLibrary& fonts)
{
    const std::vector<FontLibrary::FontInfo> systemFonts = fonts.systemFonts();

    std::string current = "Inter (default)";

    for (const FontLibrary::FontInfo& info : systemFonts) {
        if (info.path == font) current = info.displayName();
    }

    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::BeginCombo(label, current.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", ICON_MD_SEARCH " Search", &_fontFilter);

        auto matches = [&](const std::string& name) {
            if (_fontFilter.empty()) return true;

            auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); }); return s; };
            return lower(name).find(lower(_fontFilter)) != std::string::npos;
        };

        if (matches("Inter (default)") && ImGui::Selectable("Inter (default)", font.empty())) {
            font.clear();
            changed = true;
        }

        if (systemFonts.empty()) {
            ImGui::TextDisabled("Looking for the fonts of the system...");
        }

        for (const FontLibrary::FontInfo& info : systemFonts) {
            const std::string name = info.displayName();

            if (matches(name) && ImGui::Selectable(name.c_str(), info.path == font)) {
                font = info.path;
                changed = true;
            }
        }

        ImGui::EndCombo();
    }

    return changed;
}


bool Inspector::styleEditor(TextStyle& style, FontLibrary& fonts, std::string& edit)
{
    bool changed = false;

    if (!ImGui::BeginTable("##style", 2)) {
        return false;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

    propertyLabel("Font");

    if (fontCombo("##font", style.font, fonts)) {
        edit = "Change font";
        changed = true;
    }

    // Bold / italic: the variants of the family, when installed.
    propertyLabel("");

    for (const char* variant : { "Regular", "Bold", "Italic", "Bold Italic" }) {
        const std::string path = fonts.variant(style.font, variant);

        ImGui::BeginDisabled(path.empty() || path == style.font);

        if (ImGui::SmallButton(variant)) {
            style.font = path;
            edit = "Change font style";
            changed = true;
        }

        ImGui::EndDisabled();
        ImGui::SameLine();
    }

    ImGui::NewLine();

    propertyLabel("Size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::DragFloat("##size", &style.size, 0.5f, 4.f, 2000.f, "%.0f px");
    finishedEdit(edit, "Change text size");

    propertyLabel("Color");
    if (colorEdit("##color", style.color)) { changed = true; }
    finishedEdit(edit, "Change text color");

    propertyLabel("Spacing");
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::DragFloat("##spacing", &style.letterSpacing, 0.1f, -100.f, 500.f, "%.1f px");
    finishedEdit(edit, "Change letter spacing");

    propertyLabel("Outline");
    if (colorEdit("##outlineColor", style.outlineColor)) { changed = true; }
    finishedEdit(edit, "Change outline");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    // The distance field holds up to Padding pixels at BaseSize.
    const float maxOutline = (float)FontLibrary::Padding * style.size / FontLibrary::BaseSize * 0.9f;
    changed |= ImGui::SliderFloat("##outline", &style.outlineWidth, 0.f, maxOutline, "%.1f px");
    finishedEdit(edit, "Change outline");

    propertyLabel("Shadow");
    if (colorEdit("##shadowColor", style.shadowColor)) { changed = true; }
    finishedEdit(edit, "Change shadow");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    float offset[2] = { style.shadowOffsetX, style.shadowOffsetY };
    if (ImGui::DragFloat2("##shadowOffset", offset, 0.2f, -200.f, 200.f, "%.0f px")) {
        style.shadowOffsetX = offset[0];
        style.shadowOffsetY = offset[1];
        changed = true;
    }
    finishedEdit(edit, "Change shadow");

    propertyLabel("Blur");
    ImGui::SetNextItemWidth(-FLT_MIN);
    const float maxSoftness = (float)FontLibrary::Padding * style.size / FontLibrary::BaseSize * 0.9f;
    changed |= ImGui::SliderFloat("##shadowSoftness", &style.shadowSoftness, 0.f, maxSoftness, "%.1f px");
    finishedEdit(edit, "Change shadow");

    ImGui::EndTable();

    return changed;
}


void Inspector::textSection(Clip& clip, FontLibrary& fonts, const TextSelection& textSelection, std::string& edit)
{
    TextContent& text = clip.text;

    // Editing in the viewer: the style of the selected letters, applied to
    // them only.
    if (textSelection.clip == clip.id) {
        const bool range = textSelection.from != textSelection.to;

        ImGui::SeparatorText(range ? ICON_MD_FORMAT_COLOR_TEXT " Selected Letters" : ICON_MD_FORMAT_COLOR_TEXT " Whole Text");
        ImGui::TextDisabled(range ? "Changes apply to the selection." : "Select letters in the viewer to style them.");

        const TextStyle before = styleAt(text, textSelection.from + (range ? 1 : 0));
        TextStyle after = before;
        std::string styleEdit;

        ImGui::PushID("selection");
        styleEditor(after, fonts, styleEdit);
        ImGui::PopID();

        if (!(after == before)) {
            applyStyleChange(text, textSelection.from, textSelection.to, before, after);
        }

        if (!styleEdit.empty()) {
            edit = styleEdit;
        }

        ImGui::Spacing();
    }

    ImGui::SeparatorText(ICON_MD_TEXT_FIELDS " Text");

    // Paragraph
    {
        const char* alignIcons[] = { ICON_MD_FORMAT_ALIGN_LEFT, ICON_MD_FORMAT_ALIGN_CENTER, ICON_MD_FORMAT_ALIGN_RIGHT };

        for (int i = 0; i < 3; i++) {
            const bool selected = (int)text.align == i;

            if (widgets::optionButton(alignIcons[i], selected, ImVec2(ImGui::GetFrameHeight(), 0.f))) {
                text.align = (TextAlign)i;
                edit = "Align text";
            }

            ImGui::SameLine();
        }

        ImGui::SetNextItemWidth(0.5f * ImGui::GetContentRegionAvail().x);
        ImGui::DragFloat("##lineSpacing", &text.lineSpacing, 0.01f, 0.5f, 4.f, "Lines %.2f");
        finishedEdit(edit, "Change line spacing");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##boxWidth", &text.boxWidth, 2.f, 0.f, 10000.f, text.boxWidth > 0.f ? "Wrap %.0f px" : "No wrap");
        finishedEdit(edit, "Change text width");
    }

    // Spans: the text, cut in parts that each have their style.
    int removeSpan = -1;

    for (int i = 0; i < (int)text.spans.size(); i++) {
        TextSpan& span = text.spans[i];
        ImGui::PushID(i);

        const std::string header = std::string("Part ") + std::to_string(i + 1) + ": " +
            span.text.substr(0, 24) + (span.text.size() > 24 ? "..." : "") + "###span";

        if (ImGui::CollapsingHeader(header.c_str(), i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::InputTextMultiline("##text", &span.text, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.f));
            finishedEdit(edit, "Edit text");

            styleEditor(span.style, fonts, edit);

            if (text.spans.size() > 1 && ImGui::SmallButton(ICON_MD_DELETE " Remove this part")) {
                removeSpan = i;
            }
        }

        ImGui::PopID();
    }

    if (removeSpan >= 0) {
        text.spans.erase(text.spans.begin() + removeSpan);
        edit = "Remove text part";
    }

    if (ImGui::Button(ICON_MD_ADD " Add a Part with Another Style")) {
        // Same style as the last part, to change from there.
        TextSpan span = { " more", text.spans.empty() ? TextStyle() : text.spans.back().style };
        text.spans.push_back(span);
        edit = "Add text part";
    }

    // Letter animations
    ImGui::SeparatorText(ICON_MD_ANIMATION " Letter Animation");

    if (ImGui::BeginTable("##letters", 2)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        TextAnimation& animation = text.animation;

        auto combo = [&](const char* label, LetterAnimation& value) {
            propertyLabel(label);
            ImGui::SetNextItemWidth(-FLT_MIN);
            int index = (int)value;

            if (ImGui::Combo((std::string("##") + label).c_str(), &index, LetterAnimationNames, IM_ARRAYSIZE(LetterAnimationNames))) {
                value = (LetterAnimation)index;
                edit = "Change letter animation";
            }
        };

        combo("In", animation.in);
        combo("Out", animation.out);

        propertyLabel("Delay");
        ImGui::SetNextItemWidth(-FLT_MIN);
        float delay = (float)(timeToSeconds(animation.letterDelay) * 1000.0);
        if (ImGui::DragFloat("##delay", &delay, 1.f, 0.f, 2000.f, "%.0f ms / letter")) {
            animation.letterDelay = secondsToTime(delay / 1000.0);
        }
        finishedEdit(edit, "Change letter animation");

        propertyLabel("Length");
        ImGui::SetNextItemWidth(-FLT_MIN);
        float length = (float)(timeToSeconds(animation.letterDuration) * 1000.0);
        if (ImGui::DragFloat("##length", &length, 1.f, 1.f, 5000.f, "%.0f ms")) {
            animation.letterDuration = secondsToTime(std::max(1.f, length) / 1000.0);
        }
        finishedEdit(edit, "Change letter animation");

        ImGui::EndTable();
    }
}


void Inspector::sequenceSection(Project& project, std::string& edit)
{
    Sequence& sequence = project.sequence();

    ImGui::SeparatorText(ICON_MD_VIDEO_SETTINGS " Sequence");

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##name", &sequence.name);
    finishedEdit(edit, "Rename sequence");

    // Resolution
    char resolution[64];
    std::snprintf(resolution, sizeof(resolution), "%u x %u", sequence.width, sequence.height);

    ImGui::TextDisabled("Resolution");
    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::BeginCombo("##resolution", resolution)) {
        for (const ResolutionPreset& preset : ResolutionPresets) {
            const bool selected = preset.width == sequence.width && preset.height == sequence.height;

            if (ImGui::Selectable(preset.label, selected) && !selected) {
                sequence.width = preset.width;
                sequence.height = preset.height;
                edit = "Change resolution";
            }
        }

        ImGui::EndCombo();
    }

    // Frame rate
    const char* frameRateLabel = "Custom";

    for (const FrameRatePreset& preset : FrameRatePresets) {
        if (preset.rate == sequence.frameRate) frameRateLabel = preset.label;
    }

    ImGui::TextDisabled("Frame rate");
    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::BeginCombo("##frameRate", frameRateLabel)) {
        for (const FrameRatePreset& preset : FrameRatePresets) {
            const bool selected = preset.rate == sequence.frameRate;

            if (ImGui::Selectable(preset.label, selected) && !selected) {
                sequence.frameRate = preset.rate;
                edit = "Change frame rate";
            }
        }

        ImGui::EndCombo();
    }

    // Sample rate
    char sampleRate[32];
    std::snprintf(sampleRate, sizeof(sampleRate), "%u Hz", sequence.sampleRate);

    ImGui::TextDisabled("Audio sample rate");
    ImGui::SetNextItemWidth(-FLT_MIN);

    if (ImGui::BeginCombo("##sampleRate", sampleRate)) {
        for (const uint32_t rate : SampleRatePresets) {
            char label[32];
            std::snprintf(label, sizeof(label), "%u Hz", rate);

            if (ImGui::Selectable(label, rate == sequence.sampleRate) && rate != sequence.sampleRate) {
                sequence.sampleRate = rate;
                edit = "Change sample rate";
            }
        }

        ImGui::EndCombo();
    }
}
