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
    // - Blend modes mix a clip with the pictures below it:
    //       vec3 blend(vec3 base, vec3 layer)
    //   with the straight colors of both (the alpha is done around it).
    //
    // - Resampling shaders draw a picture at its size on screen, instead of
    //   the Sampling of the clip:
    //       vec4 resample(vec2 uv)
    //   with source(ivec2 pixel) (clamped to the picture), iSourceSize and
    //   iResolution (pixels of the picture, of the result).
    //
    // All get the sound of the sequence at the time of the frame (without
    // its effects), 0 for silence (-60 dB) to 1 (0 dB): iLevel, and in
    // bands iBass (below 250 Hz), iMid, iTreble (above 4 kHz); iSound is
    // the four of them. Generators also get soundBefore(seconds): iLevel
    // that long before the frame, up to 1.5 s (audio::SoundHistory), and
    // the sound in stereo by track: spectrum(track, channel, x) and
    // waveform(track, channel, x), track 0 the mix, then S1..., A1...
    // (soundTracks() of them), see audio::SoundPicture.
    //
    // Generators may draw in several passes: vec4 bufferA(vec2 uv, float
    // time), bufferB(), bufferC(), drawn before generate() into pictures of
    // their own (half floats), read with passA(uv)... (the passes before,
    // this frame) and lastA(uv)... (the frame before, for feedback; black
    // after a jump in time). See render::Compositor::cmdGenerate().
    //
    // Parameters of a generator, up to 8, are declared by comment lines:
    //       // @param Name minimum maximum default
    enum class ShaderKind
    {
        Generator,
        Transition,
        Blend,
        Resample
    };

    // "Generator", "Transition", "Blend", "Resampling".
    const char* shaderKindName(ShaderKind kind);


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
