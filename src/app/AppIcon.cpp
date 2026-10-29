#include <app/AppIcon.h>

#include <cmath>


namespace {

// Geometry, relative to the icon size (same values as in generate_icon.py).
constexpr float TileRounding = 0.22f;

// Play triangle, pointing right, slightly left of the center so that it
// looks centered.
constexpr float PlayLeft   = 0.34f;
constexpr float PlayRight  = 0.74f;
constexpr float PlayTop    = 0.20f;
constexpr float PlayBottom = 0.64f;

// Timeline clips below the triangle: two bars separated by a cut.
constexpr float ClipsTop    = 0.72f;
constexpr float ClipsBottom = 0.82f;
constexpr float ClipsLeft   = 0.18f;
constexpr float ClipsCut    = 0.56f;
constexpr float ClipsGap    = 0.05f;
constexpr float ClipsRight  = 0.82f;
constexpr float ClipRounding = 0.03f;

// Colors (same values as in generate_icon.py).
constexpr ImU32 TileColor  = IM_COL32( 18,  21,  28, 255);
constexpr ImU32 PlayColor  = IM_COL32(255, 122,   0, 255);
constexpr ImU32 Clip1Color = IM_COL32( 84, 142, 214, 255);
constexpr ImU32 Clip2Color = IM_COL32( 70, 176, 128, 255);

} // namespace


void drawAppIcon(ImDrawList* drawList, const ImVec2& pos, float size)
{
    const ImVec2 min(std::round(pos.x), std::round(pos.y));
    const ImVec2 max(min.x + size, min.y + size);

    auto at = [&](float x, float y) { return ImVec2(min.x + x * size, min.y + y * size); };

    // Tile
    drawList->AddRectFilled(min, max, TileColor, TileRounding * size);

    // Play triangle
    drawList->AddTriangleFilled(
        at(PlayLeft, PlayTop),
        at(PlayRight, 0.5f * (PlayTop + PlayBottom)),
        at(PlayLeft, PlayBottom),
        PlayColor);

    // Clips
    drawList->AddRectFilled(at(ClipsLeft, ClipsTop), at(ClipsCut - 0.5f * ClipsGap, ClipsBottom), Clip1Color, ClipRounding * size);
    drawList->AddRectFilled(at(ClipsCut + 0.5f * ClipsGap, ClipsTop), at(ClipsRight, ClipsBottom), Clip2Color, ClipRounding * size);
}
