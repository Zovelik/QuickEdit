#pragma once

#include <render/FontLibrary.h>

#include <core/Project.h>

#include <string>
#include <vector>


// Properties of the selected clip: placement with keyframes, animation
// presets, gain, and the text editor of the text clips. Edits apply to the
// project at once; draw() returns the label of an edit to record in the undo
// history when one is finished (a slider released, a button clicked).
class Inspector
{
public:
    // Characters selected in the text edited in the viewer.
    struct TextSelection
    {
        core::Id clip = core::InvalidId;
        size_t from = 0;
        size_t to = 0;
    };

    std::string draw(core::Project& project, const std::vector<core::Id>& selection, core::Time playhead,
        FontLibrary& fonts, const TextSelection& textSelection = {});

private:
    // Rows of animated values: the keyframe button, then the value.
    bool animatedRow(const char* label, core::AnimatedValue& value, core::Time localTime,
        float speed, float min, float max, const char* format, float displayScale, std::string& edit);

    void transformSection(core::Clip& clip, core::Time localTime, std::string& edit);
    void frameSection(core::Clip& clip, std::string& edit);
    void presetsSection(core::Clip& clip, std::string& edit);
    void transitionSection(core::Clip& clip, std::string& edit);
    void textSection(core::Clip& clip, FontLibrary& fonts, const TextSelection& textSelection, std::string& edit);
    bool styleEditor(core::TextStyle& style, FontLibrary& fonts, std::string& edit);
    bool fontCombo(const char* label, std::string& font, FontLibrary& fonts);

    void sequenceSection(core::Project& project, std::string& edit);

    std::string _fontFilter;
};
