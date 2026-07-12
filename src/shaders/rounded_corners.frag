#extension GL_OES_standard_derivatives : enable

#include "sdf.glsl"

uniform sampler2D texUnit;
uniform vec4 box;
uniform vec4 cornerRadius;
uniform int squircle;
uniform float modulation;

varying vec2 uv;
varying vec2 vertex;

void main(void)
{
    vec4 fragColor = texture2D(texUnit, uv);

    float radius = vertex.x < box.x
        ? (vertex.y < box.y ? cornerRadius.x : cornerRadius.w)
        : (vertex.y < box.y ? cornerRadius.y : cornerRadius.z);
    vec2 q = abs(vertex - box.xy) - box.zw + vec2(radius);
    vec2 outside = max(q, vec2(0.0));
    const float squirclePower = 3.0;
    float squircleDistance = min(max(q.x, q.y), 0.0)
        + pow(pow(outside.x, squirclePower) + pow(outside.y, squirclePower), 1.0 / squirclePower)
        - radius;
    float f = squircle != 0
        ? squircleDistance
        : sdfRoundedBox(vertex, box.xy, box.zw, cornerRadius);
    float df = fwidth(f);
    float feather = squircle != 0 ? 1.5 : 1.0;
    float alpha = clamp(0.5 - f / (df * feather), 0.0, 1.0);

    gl_FragColor = fragColor * alpha * modulation;
}
