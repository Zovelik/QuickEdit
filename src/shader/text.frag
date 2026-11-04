#version 460

// Fill, outline or shadow of a glyph from its signed distance field.

#include "text_glyph.glsl"

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 inUV;
layout(location = 1) flat in uint inGlyph;
layout(location = 0) out vec4 outColor;

// Encoding of the atlas, see FontLibrary: 128 on the outline, then this
// many units per pixel of the field.
const float OnEdgeValue = 128.0;
const float PixelDistScale = 128.0 / 12.0;


void main()
{
    const Glyph glyph = glyphs[inGlyph];

    // A box behind the text: a rounded rectangle of params.yz output pixels,
    // corners of params.x, inUV across it from 0 to 1.
    if (glyph.params.w > 1.5) {
        const vec2 size = glyph.params.yz;
        const vec2 p = (inUV - 0.5) * size;
        const float r = min(glyph.params.x, 0.5 * min(size.x, size.y));
        const vec2 d = abs(p) - 0.5 * size + r;
        const float distance = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
        const float coverage = clamp(0.5 - distance, 0.0, 1.0);

        outColor = vec4(glyph.color.rgb, glyph.color.a * coverage);
        return;
    }

    // Distance to the outline in pixels of the output, positive inside.
    const float field = texture(atlas, inUV).r * 255.0;
    const float distance = (field - OnEdgeValue) / PixelDistScale * glyph.params.z;

    const float outline = glyph.params.x;

    if (glyph.params.w > 0.5) {
        // Shadow: the glyph and its outline, blurred. The field (and the
        // quad of the glyph) reach 12 pixels of the atlas around it: a
        // softer shadow would be cut there, in boxes around the letters, so
        // it fades out within that reach.
        const float reach = 12.0 * glyph.params.z;
        const float softness = clamp(glyph.params.y, 0.5, max(0.5, 0.5 * (reach - outline)));
        const float coverage = smoothstep(-softness, softness, distance + outline);
        outColor = vec4(glyph.color.rgb, glyph.color.a * coverage);
        return;
    }

    // Fill over outline, antialiased over one pixel.
    const float fill = clamp(distance + 0.5, 0.0, 1.0);
    const float ring = (outline > 0.0) ? clamp(distance + outline + 0.5, 0.0, 1.0) : 0.0;

    const float fillAlpha = glyph.color.a * fill;
    const float ringAlpha = glyph.outlineColor.a * ring * (1.0 - fillAlpha);
    const float alpha = fillAlpha + ringAlpha;

    if (alpha <= 0.0) {
        discard;
    }

    outColor = vec4((glyph.color.rgb * fillAlpha + glyph.outlineColor.rgb * ringAlpha) / alpha, alpha);
}
