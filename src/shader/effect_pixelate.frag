#version 460

#include "effect_common.glsl"

// Large square pixels; a.x: their size in pixels of the sequence.


void main()
{
    const vec2 cell = parameters.texel * max(parameters.a.x * parameters.pixelScale, 1.0);
    const vec2 uv = (floor(inUV / cell) + 0.5) * cell;

    outColor = texture(source, uv);
}
