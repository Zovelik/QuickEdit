#pragma once

#include <core/Animation.h>
#include <core/Time.h>

#include <cstdint>
#include <string>
#include <vector>


namespace core
{
    using Id = uint64_t;


    // Effects computed on the GPU on the picture of a clip (its own pixels,
    // before its transform): the effects of the clip, then those of its
    // track. Each one is a shader of render/Compositor.
    enum class VideoEffectType
    {
        ColorCorrection,
        Blur,
        Sharpen,
        Vignette,
        ChromaKey,
        Pixelate
    };


    struct VideoEffectParameter
    {
        const char* name;
        float minimum;
        float maximum;
        float defaultValue;
        const char* format;         // Of the value times displayScale
        float displayScale = 1.f;
    };


    struct VideoEffectInfo
    {
        VideoEffectType type;
        const char* id;             // In the project files
        const char* name;
        const char* icon;
        std::vector<VideoEffectParameter> parameters;

        // The first three parameters are a color (red, green, blue in [0, 1]).
        bool colorParameter = false;
    };

    const std::vector<VideoEffectInfo>& videoEffectInfos();
    const VideoEffectInfo* videoEffectInfo(const std::string& id);


    struct VideoEffect
    {
        Id id = 0;
        std::string type;           // VideoEffectInfo::id
        bool enabled = true;

        // One per parameter of its type, in order; keyframes allowed.
        std::vector<AnimatedValue> parameters;

        const VideoEffectInfo* info() const { return videoEffectInfo(type); }
    };

    // An effect of a type with its default values.
    VideoEffect makeVideoEffect(Id id, const std::string& type);
}
