#version 460

layout(set = 0, binding = 0) uniform sampler2D layerTexture;

layout(push_constant) uniform Parameters
{
    vec4 corners01;
    vec4 corners23;
    vec4 color;
    vec4 local;
    vec4 picture;
    vec4 box;
    vec4 extra;
    vec4 shape;
} parameters;

layout(location = 0) in vec2 inLocal;
layout(location = 0) out vec4 outColor;


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

        outColor = vec4(parameters.extra.rgb, parameters.extra.a * coverage * parameters.color.a);
        return;
    }

    // Picture inside the box, border around it, antialiased over a pixel of
    // the output.
    const float d = roundedBox(inLocal - center, halfSize, radius);
    const float pixel = max(fwidth(d), 1e-4);
    const float inside = clamp(0.5 - d / pixel, 0.0, 1.0);

    const vec2 uv = (inLocal - parameters.picture.xy) / (parameters.picture.zw - parameters.picture.xy);
    const vec4 picture = texture(layerTexture, uv);

    if (border <= 0.0) {
        outColor = vec4(picture.rgb, picture.a * inside * parameters.color.a);
        return;
    }

    const float outside = clamp(0.5 - (d - border) / pixel, 0.0, 1.0);
    const vec4 color = mix(parameters.extra, picture, inside);

    outColor = vec4(color.rgb, color.a * outside * parameters.color.a);
}
