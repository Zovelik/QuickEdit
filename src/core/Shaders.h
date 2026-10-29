#pragma once

#include <filesystem>
#include <string>
#include <vector>


namespace core
{
    // GLSL shaders written by the user (or built in), compiled when used:
    //
    // - Generators make the picture of a clip:
    //       vec4 generate(vec2 uv, float time)
    //   with iResolution (pixels), iTime and iDuration (seconds) and
    //   param(i).
    //
    // - Transitions mix the outgoing and the incoming pictures:
    //       vec4 transition(vec2 uv)
    //   with getFromColor(uv), getToColor(uv), progress (0 to 1) and ratio
    //   (width / height).
    //
    // Parameters of a generator, up to 8, are declared by comment lines:
    //       // @param Name minimum maximum default
    enum class ShaderKind
    {
        Generator,
        Transition
    };


    struct ShaderParameter
    {
        std::string name;
        float minimum = 0.f;
        float maximum = 1.f;
        float defaultValue = 0.f;
    };


    struct ShaderDef
    {
        std::string id;             // "builtin:..." or "file:<name>"
        std::string name;
        ShaderKind kind = ShaderKind::Generator;
        std::string source;         // The user's code, without the wrapper
        std::vector<ShaderParameter> parameters;
    };

    const std::vector<ShaderDef>& builtinShaders();

    // From the code of a shader: its kind (from the function it defines) and
    // its parameters. False when it defines neither function.
    bool parseShader(const std::string& source, ShaderDef& shader);

    // The .glsl files of a folder (Documents/QuickEdit/Shaders), as shaders.
    std::vector<ShaderDef> loadShaderFolder(const std::filesystem::path& folder);
}
