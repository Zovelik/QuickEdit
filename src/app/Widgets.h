#pragma once

#include <app/I18n.h>

#include <imgui.h>


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
}
