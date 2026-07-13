#version 140

#include "sdf.glsl"

uniform sampler2D texUnit;
uniform vec4 box;
uniform vec4 cornerRadius;
// 0 = rounded rectangle, 1 = squircle, 2 = pointed tooltip
uniform int shape;
uniform float modulation;

in vec2 uv;
in vec2 vertex;

out vec4 fragColor;

float sdSegment(vec2 p, vec2 a, vec2 b)
{
    vec2 edge = b - a;
    vec2 offset = p - a;
    float t = clamp(dot(offset, edge) / dot(edge, edge), 0.0, 1.0);
    return length(offset - edge * t);
}

vec2 cubicPoint(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t)
{
    float u = 1.0 - t;
    return u * u * u * p0
        + 3.0 * u * u * t * p1
        + 3.0 * u * t * t * p2
        + t * t * t * p3;
}

float udCubic(vec2 p, vec2 p0, vec2 p1, vec2 p2, vec2 p3)
{
    float distance = 1e20;
    vec2 previous = p0;
    for (int i = 1; i <= 12; ++i) {
        float t = float(i) / 12.0;
        vec2 current = cubicPoint(p0, p1, p2, p3, t);
        distance = min(distance, sdSegment(p, previous, current));
        previous = current;
    }
    return distance;
}

float cubicXAtY(float y, vec2 p0, vec2 p1, vec2 p2, vec2 p3)
{
    float low = 0.0;
    float high = 1.0;
    for (int i = 0; i < 8; ++i) {
        float middle = (low + high) * 0.5;
        if (cubicPoint(p0, p1, p2, p3, middle).y < y) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return cubicPoint(p0, p1, p2, p3, (low + high) * 0.5).x;
}

float sdfTooltipArrow(vec2 p, float scale)
{
    // These are the exact logical coordinates used by append_tooltip_path().
    vec2 shoulder0 = vec2(10.3, 0.0) * scale;
    vec2 shoulder1 = vec2(7.6, 0.0) * scale;
    vec2 shoulder2 = vec2(7.0, 1.4) * scale;
    vec2 shoulder3 = vec2(5.3, 2.2) * scale;
    vec2 tip0 = vec2(1.84, 6.4) * scale;
    vec2 tip1 = vec2(1.36, 7.44) * scale;
    vec2 tip2 = vec2(0.72, 8.0) * scale;
    vec2 tip3 = vec2(0.0, 8.0) * scale;

    vec2 q = vec2(abs(p.x), p.y);
    float edgeDistance = min(
        udCubic(q, shoulder0, shoulder1, shoulder2, shoulder3),
        min(sdSegment(q, shoulder3, tip0),
            udCubic(q, tip0, tip1, tip2, tip3)));
    float baseDistance = sdSegment(q, vec2(0.0), shoulder0);
    float distance = min(edgeDistance, baseDistance);

    bool inside = false;
    if (q.y >= 0.0 && q.y <= tip3.y) {
        float boundaryX;
        if (q.y <= shoulder3.y) {
            boundaryX = cubicXAtY(q.y, shoulder0, shoulder1, shoulder2, shoulder3);
        } else if (q.y <= tip0.y) {
            float t = (q.y - shoulder3.y) / (tip0.y - shoulder3.y);
            boundaryX = mix(shoulder3.x, tip0.x, t);
        } else {
            boundaryX = cubicXAtY(q.y, tip0, tip1, tip2, tip3);
        }
        inside = q.x <= boundaryX;
    }
    return inside ? -distance : distance;
}

float sdfPointedTooltip(vec2 p, vec4 bounds, float radius)
{
    float scale = radius / 5.0;
    float inset = scale;
    float arrowHeight = 8.0 * scale;
    vec2 bodyCenter = vec2(bounds.x, bounds.y - arrowHeight * 0.5);
    vec2 bodyHalf = max(bounds.zw - vec2(inset, inset + arrowHeight * 0.5), vec2(0.0));
    float body = sdfRoundedBox(p, bodyCenter, bodyHalf, vec4(radius));

    float bodyBottom = bounds.y + bounds.w - inset - arrowHeight;
    float arrow = sdfTooltipArrow(p - vec2(bounds.x, bodyBottom), scale);
    return min(body, arrow);
}

void main(void)
{
    fragColor = texture(texUnit, uv);

    float radius = vertex.x < box.x
        ? (vertex.y < box.y ? cornerRadius.x : cornerRadius.w)
        : (vertex.y < box.y ? cornerRadius.y : cornerRadius.z);
    vec2 q = abs(vertex - box.xy) - box.zw + vec2(radius);
    vec2 outside = max(q, vec2(0.0));
    // K=0.8 cubic midpoint maps to a superellipse exponent of about 3.106.
    const float squirclePower = 3.1;
    float squircleDistance = min(max(q.x, q.y), 0.0)
        + pow(pow(outside.x, squirclePower) + pow(outside.y, squirclePower), 1.0 / squirclePower)
        - radius;
    float f = shape == 2
        ? sdfPointedTooltip(vertex, box, cornerRadius.x)
        : (shape == 1
            ? squircleDistance
            : sdfRoundedBox(vertex, box.xy, box.zw, cornerRadius));
    float df = fwidth(f);
    float feather = shape == 1 ? 1.5 : 1.0;
    float alpha = clamp(0.5 - f / (df * feather), 0.0, 1.0);

    fragColor = fragColor * alpha * modulation;
}
