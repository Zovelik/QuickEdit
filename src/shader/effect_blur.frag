#version 460

#include "effect_common.glsl"

// Gaussian blur along one direction (two passes: horizontal, vertical). The
// radius (a.x) is in pixels of the sequence; a large one samples sparsely.


void main()
{
    const float radius = max(parameters.a.x * parameters.pixelScale, 0.0);

    if (radius < 0.5) {
        outColor = texture(source, inUV);
        return;
    }

    const int taps = int(min(ceil(radius), 32.0));
    const float stride = radius / float(taps);
    const float sigma = radius / 2.5;
    const vec2 step = parameters.direction * parameters.texel * stride;

    vec4 sum = texture(source, inUV);
    float total = 1.0;

    for (int i = 1; i <= taps; i++) {
        const float x = float(i) * stride;
        const float weight = exp(-0.5 * x * x / (sigma * sigma));

        sum += (texture(source, inUV + step * float(i)) + texture(source, inUV - step * float(i))) * weight;
        total += 2.0 * weight;
    }

    outColor = sum / total;
}
