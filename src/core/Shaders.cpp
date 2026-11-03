#include <core/Shaders.h>

#include <algorithm>
#include <fstream>
#include <sstream>


namespace core
{

namespace {

ShaderDef builtin(const char* id, const char* name, ShaderKind kind, const char* source)
{
    ShaderDef shader;
    parseShader(source, shader);
    shader.id = std::string("builtin:") + id;
    shader.name = name;
    shader.kind = kind;
    return shader;
}

} // namespace


const std::vector<ShaderDef>& builtinShaders()
{
    static const std::vector<ShaderDef> shaders = {
        // Generators
        builtin("solid", "Solid Color", ShaderKind::Generator, R"(
// @param Red 0 1 0.1
// @param Green 0 1 0.1
// @param Blue 0 1 0.15
vec4 generate(vec2 uv, float time)
{
    return vec4(param(0), param(1), param(2), 1.0);
}
)"),
        builtin("gradient", "Moving Gradient", ShaderKind::Generator, R"(
// @param Speed 0 2 0.2
// @param Angle 0 360 45
// @param Hue 0 1 0.6
vec3 hue(float h)
{
    return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

vec4 generate(vec2 uv, float time)
{
    const float a = radians(param(1));
    const float x = dot(uv - 0.5, vec2(cos(a), sin(a))) + 0.5;
    const vec3 c1 = mix(vec3(0.05), hue(param(2)), 0.8);
    const vec3 c2 = mix(vec3(0.05), hue(param(2) + 0.15), 0.8);
    const float t = 0.5 + 0.5 * sin(6.2832 * (x - time * param(0)));
    return vec4(mix(c1, c2, t), 1.0);
}
)"),
        builtin("plasma", "Plasma", ShaderKind::Generator, R"(
// @param Speed 0 3 0.6
// @param Scale 1 20 6
vec4 generate(vec2 uv, float time)
{
    vec2 p = (uv - 0.5) * vec2(iResolution.x / iResolution.y, 1.0) * param(1);
    const float t = time * param(0);
    float v = sin(p.x + t) + sin(p.y + t * 1.3) + sin(p.x + p.y + t * 0.7) + sin(length(p) * 1.5 - t);
    return vec4(0.5 + 0.5 * cos(v + vec3(0.0, 2.1, 4.2)), 1.0);
}
)"),
        builtin("stars", "Starfield", ShaderKind::Generator, R"(
// @param Speed 0 3 0.5
// @param Density 0.5 4 1.5
float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

vec4 generate(vec2 uv, float time)
{
    vec2 p = (uv - 0.5) * vec2(iResolution.x / iResolution.y, 1.0);
    vec3 color = vec3(0.0);

    for (int layer = 0; layer < 4; layer++) {
        const float depth = fract(float(layer) * 0.25 + time * param(0) * 0.1);
        const float scale = mix(20.0, 1.0, depth) * param(1) * 4.0;
        const vec2 cell = floor(p * scale);
        const vec2 local = fract(p * scale) - 0.5;
        const float star = hash(cell + float(layer) * 17.0);
        const vec2 offset = vec2(hash(cell + 3.1), hash(cell + 7.7)) - 0.5;
        const float d = length(local - 0.6 * offset);
        color += vec3(smoothstep(0.08, 0.0, d) * step(0.85, star) * depth * (1.0 - depth) * 4.0);
    }

    return vec4(color, 1.0);
}
)"),
        builtin("soundrings", "Sound Rings", ShaderKind::Generator, R"(
// Rings that pulse with the sound: the bass makes them grow, the treble
// brightens them.
// @param Rings 2 30 10
// @param Hue 0 1 0.55
vec4 generate(vec2 uv, float time)
{
    vec2 p = (uv - 0.5) * vec2(iResolution.x / iResolution.y, 1.0);
    const float d = length(p) / (0.6 + 0.4 * iBass);
    const float rings = 0.5 + 0.5 * sin(d * param(0) * 6.2832 - time * 2.0);
    const vec3 hue = 0.5 + 0.5 * cos(6.2832 * (param(1) + d * 0.3 + vec3(0.0, 0.33, 0.67)));
    const float glow = smoothstep(1.0, 0.0, d) * (0.3 + 0.7 * iLevel);
    return vec4(hue * rings * glow * (0.6 + 0.8 * iTreble), 1.0);
}
)"),

        // Transitions
        builtin("circle", "Circle Reveal", ShaderKind::Transition, R"(
vec4 transition(vec2 uv)
{
    const vec2 p = (uv - 0.5) * vec2(ratio, 1.0);
    const float radius = progress * length(vec2(ratio, 1.0)) * 0.5;
    const float edge = smoothstep(radius, radius - 0.01, length(p));
    return mix(getFromColor(uv), getToColor(uv), edge);
}
)"),
        builtin("pixelize", "Pixelize", ShaderKind::Transition, R"(
vec4 transition(vec2 uv)
{
    const float size = 1.0 - 2.0 * abs(progress - 0.5);
    const vec2 cells = vec2(ratio, 1.0) * mix(400.0, 12.0, size);
    const vec2 p = (floor(uv * cells) + 0.5) / cells;
    return mix(getFromColor(p), getToColor(p), step(0.5, progress));
}
)"),
        builtin("ripple", "Ripple", ShaderKind::Transition, R"(
vec4 transition(vec2 uv)
{
    const vec2 d = uv - 0.5;
    const float wave = sin(length(d) * 40.0 - progress * 20.0) * 0.03 * sin(progress * 3.14159);
    const vec2 p = uv + normalize(d + 1e-5) * wave;
    return mix(getFromColor(p), getToColor(p), smoothstep(0.2, 0.8, progress));
}
)"),
        builtin("softwipe", "Soft Diagonal Wipe", ShaderKind::Transition, R"(
vec4 transition(vec2 uv)
{
    const float x = (uv.x * ratio + uv.y) / (ratio + 1.0);
    const float edge = smoothstep(progress * 1.2 - 0.2, progress * 1.2, x);
    return mix(getToColor(uv), getFromColor(uv), edge);
}
)"),

        // Blend modes (the separable ones of the W3C compositing spec).
        builtin("overlay", "Overlay", ShaderKind::Blend, R"(
vec3 blend(vec3 base, vec3 layer)
{
    return mix(2.0 * base * layer, 1.0 - 2.0 * (1.0 - base) * (1.0 - layer), step(0.5, base));
}
)"),
        builtin("softlight", "Soft Light", ShaderKind::Blend, R"(
vec3 blend(vec3 base, vec3 layer)
{
    const vec3 d = mix(((16.0 * base - 12.0) * base + 4.0) * base, sqrt(base), step(0.25, base));
    return mix(base - (1.0 - 2.0 * layer) * base * (1.0 - base), base + (2.0 * layer - 1.0) * (d - base), step(0.5, layer));
}
)"),
        builtin("hardlight", "Hard Light", ShaderKind::Blend, R"(
vec3 blend(vec3 base, vec3 layer)
{
    return mix(2.0 * base * layer, 1.0 - 2.0 * (1.0 - base) * (1.0 - layer), step(0.5, layer));
}
)"),
        builtin("difference", "Difference", ShaderKind::Blend, R"(
vec3 blend(vec3 base, vec3 layer)
{
    return abs(base - layer);
}
)"),
        builtin("exclusion", "Exclusion", ShaderKind::Blend, R"(
vec3 blend(vec3 base, vec3 layer)
{
    return base + layer - 2.0 * base * layer;
}
)"),
        builtin("colordodge", "Color Dodge", ShaderKind::Blend, R"(
vec3 blend(vec3 base, vec3 layer)
{
    return min(vec3(1.0), base / max(1.0 - layer, vec3(1e-4)));
}
)"),
        builtin("colorburn", "Color Burn", ShaderKind::Blend, R"(
vec3 blend(vec3 base, vec3 layer)
{
    return 1.0 - min(vec3(1.0), (1.0 - base) / max(layer, vec3(1e-4)));
}
)"),

        // Resampling
        builtin("sharpbilinear", "Sharp Bilinear (Pixel Art)", ShaderKind::Resample, R"(
// Nearest inside the pixels, a blend of one pixel of the result at their
// edges: sharp squares without the uneven sizes of Nearest.
vec4 resample(vec2 uv)
{
    const vec2 texel = uv * iSourceSize - 0.5;
    const vec2 scale = max(iResolution / iSourceSize, vec2(1.0));
    const vec2 f = fract(texel);
    const vec2 t = clamp((f - 0.5) * scale + 0.5, 0.0, 1.0);
    const ivec2 p = ivec2(floor(texel));
    const vec4 top = mix(source(p), source(p + ivec2(1, 0)), t.x);
    const vec4 bottom = mix(source(p + ivec2(0, 1)), source(p + ivec2(1, 1)), t.x);
    return mix(top, bottom, t.y);
}
)"),
    };

    return shaders;
}


const char* shaderKindName(ShaderKind kind)
{
    switch (kind) {
        case ShaderKind::Generator:  return "Generator";
        case ShaderKind::Transition: return "Transition";
        case ShaderKind::Blend:      return "Blend";
        case ShaderKind::Resample:   return "Resampling";
    }

    return "";
}


bool parseShader(const std::string& source, ShaderDef& shader)
{
    shader.source = source;
    shader.parameters.clear();

    const bool generator = source.find("generate(") != std::string::npos;
    const bool transition = source.find("transition(") != std::string::npos;
    const bool blend = source.find("blend(") != std::string::npos;
    const bool resample = source.find("resample(") != std::string::npos;

    if (!generator && !transition && !blend && !resample) {
        return false;
    }

    // The first of the functions it defines (a shader may call the others).
    shader.kind = generator ? ShaderKind::Generator
        : transition ? ShaderKind::Transition
        : resample ? ShaderKind::Resample : ShaderKind::Blend;

    std::istringstream lines(source);
    std::string line;

    while (std::getline(lines, line) && shader.parameters.size() < 8) {
        const size_t at = line.find("@param");

        if (at == std::string::npos || line.find("//") > at) {
            continue;
        }

        std::istringstream words(line.substr(at + 6));
        ShaderParameter parameter;

        if (words >> parameter.name >> parameter.minimum >> parameter.maximum >> parameter.defaultValue) {
            shader.parameters.push_back(parameter);
        }
    }

    return true;
}


std::vector<ShaderDef> loadShaderFolder(const std::filesystem::path& folder)
{
    std::vector<ShaderDef> shaders;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".glsl") {
            continue;
        }

        std::ifstream file(entry.path(), std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();

        ShaderDef shader;

        if (parseShader(text.str(), shader)) {
            const std::u8string stem = entry.path().stem().u8string();
            shader.name = std::string(stem.begin(), stem.end());
            shader.id = "file:" + shader.name;
            shaders.push_back(std::move(shader));
        }
    }

    std::sort(shaders.begin(), shaders.end(), [](const ShaderDef& a, const ShaderDef& b) { return a.name < b.name; });

    return shaders;
}

}
