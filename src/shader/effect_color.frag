#version 460

#include "effect_common.glsl"

// Color correction: brightness, contrast, saturation, gamma (a), then
// temperature and hue (b).


vec3 rotateHue(vec3 color, float degrees)
{
    // Rotation around the grey axis.
    const float angle = radians(degrees);
    const vec3 k = vec3(0.57735);
    const float c = cos(angle);
    return color * c + cross(k, color) * sin(angle) + k * dot(k, color) * (1.0 - c);
}


void main()
{
    vec4 color = texture(source, inUV);
    vec3 rgb = color.rgb;

    rgb += parameters.a.x;
    rgb = (rgb - 0.5) * parameters.a.y + 0.5;

    const float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(luma), rgb, parameters.a.z);

    rgb = pow(max(rgb, vec3(0.0)), vec3(1.0 / max(parameters.a.w, 0.01)));

    // Warmer: more red, less blue.
    rgb *= vec3(1.0 + 0.3 * parameters.b.x, 1.0, 1.0 - 0.3 * parameters.b.x);

    if (parameters.b.y != 0.0) {
        rgb = rotateHue(rgb, parameters.b.y);
    }

    outColor = vec4(clamp(rgb, 0.0, 1.0), color.a);
}
