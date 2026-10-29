#version 460

// The whole target, as a triangle strip of 4 vertices: the passes of the
// video effects (shader/effect_*.frag).

layout(location = 0) out vec2 outUV;


void main()
{
    const vec2 corners[4] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
    outUV = 0.5 * corners[gl_VertexIndex] + 0.5;
}
