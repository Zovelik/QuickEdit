// Instance data of the glyphs, shared by text.vert and text.frag. Matches
// GlyphInstance in Compositor.cpp.

struct Glyph
{
    vec4 corners01;     // Normalized device coordinates: top left, top right
    vec4 corners23;     // bottom left, bottom right
    vec4 uv;            // Atlas rectangle: u0, v0, u1, v1
    vec4 color;         // Fill, or shadow color
    vec4 outlineColor;
    vec4 params;        // x: outline width (px), y: shadow softness (px),
                        // z: output pixels per field pixel, w: 1 for a shadow
};

layout(std430, set = 0, binding = 1) readonly buffer Glyphs
{
    Glyph glyphs[];
};
