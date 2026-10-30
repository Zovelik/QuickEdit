#version 460

layout(set = 0, binding = 0) uniform sampler2D layerTexture;

layout(push_constant) uniform Parameters
{
    vec4 corners01;
    vec4 corners23;
    vec4 color;         // x: the filter (2 bicubic, 3 Lanczos, else the sampler's); w: opacity
    vec4 local;
    vec4 picture;
    vec4 box;
    vec4 extra;
    vec4 shape;
} parameters;

layout(location = 0) in vec2 inLocal;
layout(location = 0) out vec4 outColor;

// The blending of the pipeline (core::BlendMode): Normal mixes the straight
// color by its alpha; the others need the color premultiplied, Darken over
// white where transparent (see Compositor::createPipeline()).
layout(constant_id = 0) const int blendMode = 0;

const int BlendNormal = 0;
const int BlendDarken = 5;

vec4 blended(vec4 color)
{
    if (blendMode == BlendNormal) {
        return color;
    }

    if (blendMode == BlendDarken) {
        return vec4(mix(vec3(1.0), color.rgb, color.a), color.a);
    }

    return vec4(color.rgb * color.a, color.a);
}


// Weights of the filters read here, at a distance in texels.
float catmullRom(float x)
{
    x = abs(x);

    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

float lanczos3(float x)
{
    if (abs(x) < 1e-5) return 1.0;
    if (abs(x) >= 3.0) return 0.0;

    const float p = 3.14159265 * x;
    return 3.0 * sin(p) * sin(p / 3.0) / (p * p);
}

// The picture at uv with a filter of 4 x 4 (bicubic) or 6 x 6 (Lanczos)
// texels, the edges repeated.
vec4 filtered(vec2 uv, bool lanczos)
{
    const ivec2 size = textureSize(layerTexture, 0);
    const vec2 position = uv * vec2(size) - 0.5;
    const vec2 base = floor(position);
    const vec2 f = position - base;
    const int radius = lanczos ? 3 : 2;

    vec4 sum = vec4(0.0);
    float total = 0.0;

    for (int y = 1 - radius; y <= radius; y++) {
        for (int x = 1 - radius; x <= radius; x++) {
            const vec2 d = vec2(x, y) - f;
            const float w = lanczos ? lanczos3(d.x) * lanczos3(d.y) : catmullRom(d.x) * catmullRom(d.y);
            const ivec2 texel = clamp(ivec2(base) + ivec2(x, y), ivec2(0), size - 1);

            sum += texelFetch(layerTexture, texel, 0) * w;
            total += w;
        }
    }

    return clamp(sum / total, 0.0, 1.0);
}


// Signed distance to a rectangle of half size h with corners of radius r.
float roundedBox(vec2 p, vec2 h, float r)
{
    r = min(r, min(h.x, h.y));
    const vec2 d = abs(p) - h + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}


void main()
{
    const vec2 center = 0.5 * (parameters.box.xy + parameters.box.zw);
    const vec2 halfSize = 0.5 * (parameters.box.zw - parameters.box.xy);
    const float radius = parameters.shape.x;
    const float border = parameters.shape.y;

    // Shadow: of the picture and its border, blurred over the softness.
    if (parameters.shape.w > 0.5) {
        const float d = roundedBox(inLocal - center, halfSize + border, radius + border);
        const float softness = max(parameters.shape.z, 0.5);
        const float coverage = 1.0 - smoothstep(-softness, softness, d);

        outColor = blended(vec4(parameters.extra.rgb, parameters.extra.a * coverage * parameters.color.a));
        return;
    }

    // Picture inside the box, border around it, antialiased over a pixel of
    // the output.
    const float d = roundedBox(inLocal - center, halfSize, radius);
    const float pixel = max(fwidth(d), 1e-4);
    const float inside = clamp(0.5 - d / pixel, 0.0, 1.0);

    const vec2 uv = (inLocal - parameters.picture.xy) / (parameters.picture.zw - parameters.picture.xy);
    const int filterMode = int(parameters.color.x + 0.5);
    const vec4 picture = (filterMode == 2 || filterMode == 3) ? filtered(uv, filterMode == 3) : texture(layerTexture, uv);

    if (border <= 0.0) {
        outColor = blended(vec4(picture.rgb, picture.a * inside * parameters.color.a));
        return;
    }

    const float outside = clamp(0.5 - (d - border) / pixel, 0.0, 1.0);
    const vec4 color = mix(parameters.extra, picture, inside);

    outColor = blended(vec4(color.rgb, color.a * outside * parameters.color.a));
}
