#version 460

// Quad of a video or image layer, drawn as a triangle strip of 4 vertices
// whose corners (top left, top right, bottom left, bottom right) are already
// transformed to normalized device coordinates. The quad covers the
// rectangle "local" of the layer (its pixels, centered on the picture),
// which the fragment shader shapes: crop, rounded corners, border, shadow.

layout(push_constant) uniform Parameters
{
    vec4 corners01;     // xy: top left, zw: top right
    vec4 corners23;     // xy: bottom left, zw: bottom right
    vec4 color;         // Opacity in alpha
    vec4 local;         // Rectangle covered by the quad: x0, y0, x1, y1
    vec4 picture;       // Rectangle of the whole picture
    vec4 box;           // Rectangle shown (cropped picture, or the shadow)
    vec4 extra;         // Color of the border, or of the shadow
    vec4 shape;         // Corner radius, border width, shadow softness, 1 for the shadow
} parameters;

layout(location = 0) out vec2 outLocal;


void main()
{
    const vec2 corners[4] = vec2[](
        parameters.corners01.xy, parameters.corners01.zw,
        parameters.corners23.xy, parameters.corners23.zw);

    const vec2 uvs[4] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));

    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
    outLocal = mix(parameters.local.xy, parameters.local.zw, uvs[gl_VertexIndex]);
}
