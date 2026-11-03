#pragma once

#include <app/I18n.h>
#include <core/Animation.h>

#include <fonts/IconsMaterialDesign.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <string>


// Small widgets shared by the panels, for a consistent look.
namespace widgets
{
    // The keyframe controls of an animated value, at the time t of the
    // playhead (in the time of the value): go to the previous keyframe, add
    // or remove the one at t (filled at a keyframe, outlined when animated
    // between keyframes), go to the next one. The edit made gets its label
    // in edit; a move asked gives the offset from t in goBy.
    inline void keyframeButtons(core::AnimatedValue& value, core::Time t, std::string& edit, core::Time& goBy)
    {
        const bool atKey = value.keyAt(t) >= 0;
        const char* icon = atKey ? ICON_MD_RADIO_BUTTON_CHECKED : (value.animated() ? ICON_MD_RADIO_BUTTON_UNCHECKED : ICON_MD_ADD_CIRCLE_OUTLINE);

        const core::Keyframe* previous = nullptr;
        const core::Keyframe* next = nullptr;

        for (const core::Keyframe& key : value.keys) {
            if (key.time < t) previous = &key;
            if (key.time > t && !next) next = &key;
        }

        const float arrowWidth = 0.75f * ImGui::GetFrameHeight();

        ImGui::PushStyleVarX(ImGuiStyleVar_ItemSpacing, 0.f);
        ImGui::PushStyleVarX(ImGuiStyleVar_FramePadding, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));

        // Only shown when animated: nothing to go to otherwise.
        ImGui::BeginDisabled(!previous);

        if (ImGui::Button(value.animated() ? ICON_MD_CHEVRON_LEFT "##previousKey" : " ##previousKey", ImVec2(arrowWidth, 0.f)) && previous) {
            goBy = previous->time - t;
        }

        ImGui::EndDisabled();

        if (previous && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tr("Previous keyframe"));
        }

        ImGui::SameLine();

        if (value.animated()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 170, 60, 255));
        }

        const bool toggled = ImGui::Button(icon, ImVec2(ImGui::GetFrameHeight(), 0.f));

        if (value.animated()) {
            ImGui::PopStyleColor();
        }

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("%s", atKey ? tr("Remove the keyframe") : tr("Add a keyframe at the playhead"));
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!next);

        if (ImGui::Button(value.animated() ? ICON_MD_CHEVRON_RIGHT "##nextKey" : " ##nextKey", ImVec2(arrowWidth, 0.f)) && next) {
            goBy = next->time - t;
        }

        ImGui::EndDisabled();

        if (next && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tr("Next keyframe"));
        }

        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        if (toggled) {
            if (atKey) {
                value.removeKey(t);
                edit = "Remove keyframe";
            }
            else {
                value.addKey(t);
                edit = "Add keyframe";
            }
        }
    }


    // A button of a set of choices (segmented control): the chosen one in
    // the accent color, the others neutral like the input fields.
    inline bool optionButton(const char* label, bool selected, const ImVec2& size = ImVec2(0.f, 0.f))
    {
        const ImGuiStyle& style = ImGui::GetStyle();

        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_ButtonActive]);
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_FrameBg]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.Colors[ImGuiCol_FrameBgHovered]);
        }

        const bool pressed = ImGui::Button(tr(label), size);
        ImGui::PopStyleColor(2);

        return pressed && !selected;
    }

    // Flat button of the toolbars of the panels, as tall as the other widgets
    // of the line (combos, sliders); square when it is an icon alone. The
    // selected one (a tool, a toggle) is highlighted. Stays on the same line.
    inline bool panelButton(const char* label, const char* tooltip, bool selected = false, bool enabled = true)
    {
        // An icon is one character of the private use area: 3 bytes in UTF-8.
        const char* end = ImGui::FindRenderedTextEnd(label);
        const bool iconOnly = (end - label) == 3 && (unsigned char)label[0] == 0xEE;
        const ImVec2 size(iconOnly ? ImGui::GetFrameHeight() : 0.f, ImGui::GetFrameHeight());

        ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button(tr(label), size);
        ImGui::EndDisabled();
        ImGui::PopStyleColor();

        if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tr(tooltip));
        }

        ImGui::SameLine();
        return pressed;
    }
}
