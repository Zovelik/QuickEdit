#include <core/VideoEffects.h>


namespace core
{

// Icons of the Material Design font (see fonts/IconsMaterialDesign.h).
#define ICON_COLOR "\xee\x90\x8a"       // palette
#define ICON_BLUR "\xee\x8e\xa5"        // blur_on
#define ICON_SHARPEN "\xee\x8f\x88"     // details
#define ICON_VIGNETTE "\xee\x90\xb5"    // vignette
#define ICON_KEY "\xee\x90\xa1"         // texture
#define ICON_PIXELATE "\xee\x8f\xac"    // grid_on


const std::vector<VideoEffectInfo>& videoEffectInfos()
{
    static const std::vector<VideoEffectInfo> infos = {
        { VideoEffectType::ColorCorrection, "color", "Color Correction", ICON_COLOR, {
            { "Brightness", -1.f, 1.f, 0.f, "%+.0f %%", 100.f },
            { "Contrast", 0.f, 3.f, 1.f, "%.0f %%", 100.f },
            { "Saturation", 0.f, 3.f, 1.f, "%.0f %%", 100.f },
            { "Gamma", 0.2f, 3.f, 1.f, "%.2f" },
            { "Temperature", -1.f, 1.f, 0.f, "%+.0f", 100.f },
            { "Hue", -180.f, 180.f, 0.f, "%+.0f\xc2\xb0" },
        } },
        { VideoEffectType::Blur, "blur", "Blur", ICON_BLUR, {
            { "Radius", 0.f, 200.f, 10.f, "%.1f px" },
        } },
        { VideoEffectType::Sharpen, "sharpen", "Sharpen", ICON_SHARPEN, {
            { "Amount", 0.f, 3.f, 0.5f, "%.0f %%", 100.f },
        } },
        { VideoEffectType::Vignette, "vignette", "Vignette", ICON_VIGNETTE, {
            { "Amount", 0.f, 1.f, 0.5f, "%.0f %%", 100.f },
            { "Size", 0.f, 1.f, 0.6f, "%.0f %%", 100.f },
            { "Softness", 0.01f, 1.f, 0.5f, "%.0f %%", 100.f },
        } },
        { VideoEffectType::ChromaKey, "chromakey", "Chroma Key", ICON_KEY, {
            { "Red", 0.f, 1.f, 0.f, "%.2f" },
            { "Green", 0.f, 1.f, 1.f, "%.2f" },
            { "Blue", 0.f, 1.f, 0.f, "%.2f" },
            { "Threshold", 0.f, 1.f, 0.25f, "%.0f %%", 100.f },
            { "Softness", 0.f, 1.f, 0.1f, "%.0f %%", 100.f },
            { "Spill", 0.f, 1.f, 0.5f, "%.0f %%", 100.f },
        }, true },
        { VideoEffectType::Pixelate, "pixelate", "Pixelate", ICON_PIXELATE, {
            { "Size", 1.f, 200.f, 16.f, "%.0f px" },
        } },
    };

    return infos;
}


const VideoEffectInfo* videoEffectInfo(const std::string& id)
{
    for (const VideoEffectInfo& info : videoEffectInfos()) {
        if (id == info.id) return &info;
    }

    return nullptr;
}


VideoEffect makeVideoEffect(Id id, const std::string& type)
{
    VideoEffect effect;
    effect.id = id;
    effect.type = type;

    if (const VideoEffectInfo* info = videoEffectInfo(type)) {
        for (const VideoEffectParameter& parameter : info->parameters) {
            effect.parameters.emplace_back(parameter.defaultValue);
        }
    }

    return effect;
}

}
