#version 460

// Glyphs of a text, one instance per glyph: a quad of 4 vertices (triangle
// strip) whose corners are already transformed to normalized device
// coordinates (layer transform and letter animation).

#include "text_glyph.glsl"

layout(location = 0) out vec2 outUV;
layout(location = 1) flat out uint outGlyph;


void main()
{
    const Glyph glyph = glyphs[gl_InstanceIndex];

    const vec2 corners[4] = vec2[](glyph.corners01.xy, glyph.corners01.zw, glyph.corners23.xy, glyph.corners23.zw);
    const vec2 uvs[4] = vec2[](glyph.uv.xy, glyph.uv.zy, glyph.uv.xw, glyph.uv.zw);

    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
    outUV = uvs[gl_VertexIndex];
    outGlyph = gl_InstanceIndex;
}
