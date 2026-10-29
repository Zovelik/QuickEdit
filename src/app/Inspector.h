#pragma once

#include <render/FontLibrary.h>

#include <core/Project.h>

#include <functional>
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

    // The shaders of the user (offered with the built-in ones), why one does
    // not compile, and the folder they are in.
    const std::vector<core::ShaderDef>* userShaders = nullptr;
    std::function<std::string(const std::string& shaderId)> shaderError;
    std::function<void()> openShaderFolder;

    // A shader of the user opened in the shader editor, and a new one made
    // from an example (nullptr when it could not be written).
    std::function<void(const std::string& shaderId)> editShader;
    std::function<const core::ShaderDef*(core::ShaderKind kind)> newShader;

    // Name, resolution, frame rate and sample rate of the sequence (also
    // in the Project Properties window).
    void sequenceSection(core::Project& project, std::string& edit);

    // A list of video effects (of a clip or a track): on / off, order,
    // parameters with keyframes at localTime, and the effects to add.
    void videoEffectsSection(core::Project& project, std::vector<core::VideoEffect>& effects, core::Time localTime, std::string& edit);

private:
    // Rows of animated values: the keyframe button, then the value.
    bool animatedRow(const char* label, core::AnimatedValue& value, core::Time localTime,
        float speed, float min, float max, const char* format, float displayScale, std::string& edit);

    void transformSection(core::Clip& clip, core::Time localTime, std::string& edit);
    void frameSection(core::Clip& clip, std::string& edit);
    void presetsSection(core::Clip& clip, std::string& edit);
    void transitionSection(core::Project& project, core::Clip& clip, std::string& edit);
    void generatorSection(core::Project& project, core::Clip& clip, core::Time localTime, std::string& edit);
    void subtitleSection(core::Project& project, core::Clip& clip, std::string& edit);

public:
    // Size, color, outline and place of the subtitles of the sequence (also
    // in the Project Properties window).
    void subtitleStyleSection(core::Project& project, std::string& edit);

private:
    void textSection(core::Clip& clip, FontLibrary& fonts, const TextSelection& textSelection, std::string& edit);
    bool styleEditor(core::TextStyle& style, FontLibrary& fonts, std::string& edit);
    bool fontCombo(const char* label, std::string& font, FontLibrary& fonts);

    std::string _fontFilter;
};
