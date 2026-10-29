#version 460

#include "effect_common.glsl"

// Unsharp mask on the neighbours of each pixel; a.x: amount.


void main()
{
    const vec4 center = texture(source, inUV);
    const vec2 t = parameters.texel * max(parameters.pixelScale, 1.0);

    const vec4 around = 0.25 * (
        texture(source, inUV + vec2(t.x, 0.0)) + texture(source, inUV - vec2(t.x, 0.0)) +
        texture(source, inUV + vec2(0.0, t.y)) + texture(source, inUV - vec2(0.0, t.y)));

    outColor = vec4(clamp(center.rgb + (center.rgb - around.rgb) * parameters.a.x, 0.0, 1.0), center.a);
}
