#pragma once

#include <app/I18n.h>

#include <imgui.h>
#include <imgui_internal.h>


// Small widgets shared by the panels, for a consistent look.
namespace widgets
{
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
