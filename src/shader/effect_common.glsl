// Shared by the passes of the video effects: the picture before the
// effect, and the parameters of the effect (core/VideoEffects.cpp).

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Parameters
{
    vec4 a;             // Parameters 0 to 3 of the effect
    vec4 b;             // Parameters 4 to 7
    vec2 texel;         // Size of a pixel of the picture, in texture coordinates
    vec2 direction;     // Of a separable pass (blur)
    float pixelScale;   // Pixels of the picture per pixel of the sequence
    vec4 visible;       // The part not cropped (left, top, right, bottom), in texture coordinates
} parameters;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
