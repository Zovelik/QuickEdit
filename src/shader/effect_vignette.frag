#version 460

#include "effect_common.glsl"

// Darker edges; a: amount, size of the clear center, softness.


void main()
{
    const vec4 color = texture(source, inUV);

    // Round whatever the shape of the picture, on the part left by the
    // crop.
    const vec2 visibleSize = max(parameters.visible.zw - parameters.visible.xy, vec2(0.001));
    const float aspect = (visibleSize.x * parameters.texel.y) / (visibleSize.y * parameters.texel.x);
    vec2 p = ((inUV - parameters.visible.xy) / visibleSize - 0.5) * 2.0;
    p.x *= aspect / max(aspect, 1.0);
    p.y /= max(aspect, 1.0);

    const float distance = length(p) / 1.4142;
    const float size = parameters.a.y;
    const float softness = max(parameters.a.z, 0.01);
    const float dark = smoothstep(size, size + softness, distance) * parameters.a.x;

    outColor = vec4(color.rgb * (1.0 - dark), color.a);
}
