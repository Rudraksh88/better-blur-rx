#version 140

#include "sdf.glsl"

uniform sampler2D texUnit;
uniform vec4 box;
uniform vec4 cornerRadius;
// 0 = rounded rectangle, 1 = squircle, 2 = pointed tooltip,
// 3 = squircle with square bottom corners
uniform int shape;
uniform float squircleExponent;
uniform float shapeInset;
// x = arrow height, y = arrow half-width, z = shoulder, w = tip radius
uniform vec4 tooltipGeometry;
// x = inset, y = derivative-AA feather,
// z = arrow centre offset from the body centre (device px, pre-clamped),
// w = pointed body squircle exponent; 2 keeps a rounded rectangle
uniform vec4 tooltipStyle;
uniform float modulation;

in vec2 uv;
in vec2 vertex;

out vec4 fragColor;

float sdSegment(vec2 p, vec2 a, vec2 b)
{
    vec2 edge = b - a;
    vec2 offset = p - a;
    float t = clamp(dot(offset, edge) / max(dot(edge, edge), 0.000001), 0.0, 1.0);
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

float sdfTooltipArrow(vec2 p, vec4 geometry)
{
    float arrowHeight = geometry.x;
    float arrowHalf = geometry.y;
    float shoulder = geometry.z;
    float tipRadius = geometry.w;
    vec2 shoulder0 = vec2(arrowHalf + shoulder, 0.0);
    float flank = max(length(vec2(arrowHalf, arrowHeight)), 0.000001);
    vec2 unitFlank = vec2(-arrowHalf, arrowHeight) / flank;
    vec2 shoulder1 = vec2(arrowHalf + shoulder / 3.0, 0.0);
    vec2 shoulder2 = vec2(arrowHalf, 0.0) + unitFlank * (shoulder / 3.0);
    vec2 shoulder3 = vec2(arrowHalf, 0.0) + unitFlank * shoulder;
    vec2 tip0 = vec2(0.0, arrowHeight) - unitFlank * tipRadius;
    float tipCircleRadius = tipRadius * arrowHalf / max(arrowHeight, 0.000001);
    float tipHalfArc = atan(arrowHeight, arrowHalf);
    float tipControl = 4.0 / 3.0 * tipCircleRadius * tan(tipHalfArc / 4.0);
    float apexHeight = max(
        0.0, arrowHeight - (flank - arrowHalf) / arrowHeight * tipRadius);
    vec2 tip1 = tip0 + unitFlank * tipControl;
    vec2 tip2 = vec2(tipControl, apexHeight);
    vec2 tip3 = vec2(0.0, apexHeight);

    vec2 q = vec2(abs(p.x), p.y);
    float edgeDistance = min(
        udCubic(q, shoulder0, shoulder1, shoulder2, shoulder3),
        min(sdSegment(q, shoulder3, tip0),
            udCubic(q, tip0, tip1, tip2, tip3)));
    bool inside = false;
    if (q.y >= 0.0 && q.y <= tip3.y) {
        float boundaryX;
        if (shoulder3.y > 0.000001 && q.y <= shoulder3.y) {
            boundaryX = cubicXAtY(q.y, shoulder0, shoulder1, shoulder2, shoulder3);
        } else if (q.y <= tip0.y || tipRadius <= 0.000001) {
            float t = clamp(
                (q.y - shoulder3.y) / max(tip0.y - shoulder3.y, 0.000001),
                0.0,
                1.0);
            boundaryX = mix(shoulder3.x, tip0.x, t);
        } else {
            boundaryX = cubicXAtY(q.y, tip0, tip1, tip2, tip3);
        }
        inside = q.x <= boundaryX;
    }
    // This arrow is unioned with the rounded body, so its base is deliberately
    // open. Treating the base as another zero-distance edge leaves a 50% alpha
    // line inside the union whenever the junction lands on a sample row.
    return inside ? -edgeDistance : edgeDistance;
}

vec3 sdfPointedTooltipParts(vec2 p, vec4 bounds, float radius)
{
    float inset = tooltipStyle.x;
    float arrowHeight = tooltipGeometry.x;
    vec2 bodyCenter = vec2(bounds.x, bounds.y - arrowHeight * 0.5);
    vec2 bodyHalf = max(bounds.zw - vec2(inset, inset + arrowHeight * 0.5), vec2(0.0));
    float bodyRadius = min(radius, min(bodyHalf.x, bodyHalf.y));
    float body = sdfRoundedBox(p, bodyCenter, bodyHalf, vec4(bodyRadius));
    if (tooltipStyle.w > 2.0001) {
        vec2 q = abs(p - bodyCenter) - bodyHalf + vec2(bodyRadius);
        vec2 outside = max(q, vec2(0.0));
        body = min(max(q.x, q.y), 0.0)
            + pow(pow(outside.x, tooltipStyle.w) + pow(outside.y, tooltipStyle.w),
                  1.0 / tooltipStyle.w)
            - bodyRadius;
    }

    if (arrowHeight <= 0.000001) {
        return vec3(body, 1e20, 0.0);
    }

    float bodyBottom = bounds.y + bounds.w - inset - arrowHeight;
    vec2 local = p - vec2(bounds.x + tooltipStyle.z, bodyBottom);
    // Curved-arrow distance is much more expensive than the rounded body.
    // Evaluate it only near its conservative bounding box. Compute derivatives
    // before branching, and leave two derivative-neighbour pixels around the
    // coverage box: the sentinel transition then cannot affect visible AA.
    vec2 pixel = max(abs(dFdx(p)) + abs(dFdy(p)), vec2(0.0001));
    vec2 padding = pixel * max(2.0, tooltipStyle.y * 2.0);
    vec2 outsideArrow = vec2(
        abs(local.x) - (tooltipGeometry.y + tooltipGeometry.z),
        max(-local.y, local.y - arrowHeight));
    bool coversArrow = all(lessThanEqual(outsideArrow, padding));
    float arrow = 1e20;
    if (all(lessThanEqual(outsideArrow, padding + 2.0 * pixel))) {
        arrow = sdfTooltipArrow(local, tooltipGeometry);
    }
    return vec3(body, arrow, coversArrow ? 1.0 : 0.0);
}

float antialiasedCoverage(float distance, float feather)
{
    float width = max(fwidth(distance) * feather, 0.0001);
    return clamp(0.5 - distance / width, 0.0, 1.0);
}

void main(void)
{
    fragColor = texture(texUnit, uv);

    float radius = vertex.x < box.x
        ? (vertex.y < box.y ? cornerRadius.x : cornerRadius.w)
        : (vertex.y < box.y ? cornerRadius.y : cornerRadius.z);
    bool squircleMask = shape == 1 || shape == 3;
    // Insets normally shrink all four sides. A flat-bottom squircle instead
    // keeps the lower edge on the allocation boundary while insetting its top
    // and sides; shift the center by half the one-sided vertical inset.
    vec2 shapeCenter = box.xy + vec2(0.0, shape == 3 ? shapeInset * 0.5 : 0.0);
    vec2 squircleInset = shape == 3
        ? vec2(shapeInset, shapeInset * 0.5)
        : vec2(squircleMask ? shapeInset : 0.0);
    vec2 shapeHalf = max(box.zw - squircleInset, vec2(0.0));
    vec2 q = abs(vertex - shapeCenter) - shapeHalf + vec2(radius);
    vec2 outside = max(q, vec2(0.0));
    // K=0.8 cubic midpoint maps to a superellipse exponent of about 3.106.
    float squirclePower = squircleExponent;
    float squircleDistance = min(max(q.x, q.y), 0.0)
        + pow(pow(outside.x, squirclePower) + pow(outside.y, squirclePower), 1.0 / squirclePower)
        - radius;
    float alpha;
    if (shape == 2) {
        vec3 tooltipParts = sdfPointedTooltipParts(vertex, box, cornerRadius.x);
        float bodyAlpha = antialiasedCoverage(tooltipParts.x, tooltipStyle.y);
        float arrowAlpha = antialiasedCoverage(tooltipParts.y, tooltipStyle.y) * tooltipParts.z;
        // Union coverage once in this fragment. max() neither double-composites
        // the overlap nor exposes a derivative seam where the primitives meet.
        alpha = max(bodyAlpha, arrowAlpha);
    } else {
        float f = squircleMask
            ? squircleDistance
            : sdfRoundedBox(vertex, box.xy, box.zw, cornerRadius);
        alpha = antialiasedCoverage(f, squircleMask ? 1.5 : 1.0);
    }

    fragColor = fragColor * alpha * modulation;
}
