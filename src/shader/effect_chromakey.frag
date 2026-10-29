#version 460

#include "effect_common.glsl"

// Chroma key: transparent where the color is close to the key (a.rgb), in
// chroma only so that shadows on a green screen go too. a.w: threshold,
// b.x: softness, b.y: spill removal.


vec2 chroma(vec3 rgb)
{
    return vec2(
        -0.1146 * rgb.r - 0.3854 * rgb.g + 0.5 * rgb.b,
        0.5 * rgb.r - 0.4542 * rgb.g - 0.0458 * rgb.b);
}


void main()
{
    const vec4 color = texture(source, inUV);
    const vec3 key = parameters.a.rgb;

    const float distance = length(chroma(color.rgb) - chroma(key)) * 2.0;
    const float threshold = parameters.a.w;
    const float softness = max(parameters.b.x, 0.001);
    const float alpha = smoothstep(threshold, threshold + softness, distance);

    // Spill: the key color left on the edges, pulled towards grey.
    vec3 rgb = color.rgb;
    const float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    const float spill = (1.0 - alpha) * parameters.b.y + parameters.b.y * 0.3 * (1.0 - smoothstep(threshold, threshold + 4.0 * softness, distance));
    rgb = mix(rgb, vec3(luma), clamp(spill, 0.0, 1.0));

    outColor = vec4(rgb, color.a * alpha);
}
