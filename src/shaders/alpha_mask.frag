// Final composite of a cached blur, shaped by the window's own alpha channel.
// See alpha_mask_core.frag for the uniform contract.
uniform sampler2D texUnit;
uniform sampler2D alphaTex0;
uniform sampler2D alphaTex1;
uniform sampler2D alphaTex2;
uniform sampler2D alphaTex3;
uniform mat4 alphaTexMatrix[4];
uniform vec4 surfaceBox[4];
uniform vec4 bufferSource[4];
// Per surface: one texel in texture coordinates, for the silhouette kernel.
uniform vec2 texelSize[4];
uniform int alphaCount;
// 0 = threshold: coverage = alpha / alphaThreshold, strength follows the fill.
// 1 = silhouette: full coverage inside any fill above alphaFloor; at an edge
//     against transparency coverage = alpha / neighbourhood max, i.e. the
//     client's own antialiasing. Strength never depends on the fill alpha.
// 2 = threshold with an exact rim (per-surface kind "alpha-threshold"): as
//     0 inside, but on the antialiased rim against transparency coverage =
//     alpha / the outermost layer's alpha (see rimCoverage).
uniform int alphaMode;
uniform float alphaThreshold;
uniform float alphaFloor;
// Coverage is capped by this rect's share of the pixel (x0, y0, x1, y1, in
// the same device space as surfaceBox). Clips nothing unless a published
// "alpha-threshold" shape gives an inset: then it is the surface inset to
// where its paint starts, which keeps straight edges exact when the
// outermost painted layer is thinner than a pixel (no texel shows its full
// alpha, so the rim's alpha / local maximum over-covers there).
uniform vec4 clipBox;
uniform float modulation;

varying vec2 uv;
varying vec2 vertex;

// Mode 2. On the rim a pixel's alpha is the share of it the outermost
// painted layer covers times that layer's alpha (a dark ring, say), which
// is usually above the fill's T: alpha / T over-covers the rim, a
// half-covered ring pixel getting full blur. Divide by that layer's alpha
// instead, read as the 5x5 maximum. Rim pixels: a transparent texel in the
// 3x3 neighbourhood or, below the fill (alpha < T, with slack for a fill
// drawn at T and rounded to 8 bits), in the 5x5 one, which reaches the
// wider ramps of rotated or resampled content. Fill pixels sit two texels
// or more inside a ring and keep alpha / T. Never dividing by less than T
// keeps translucent soft shadows at alpha / T.
// Taps past the buffer's edge read as transparent rather than as the
// clamped edge texel, so a surface painted up to its border (the dock
// plate) still has a rim there. lo / hi bound the buffer in texture space.
float rimTap(sampler2D tex, vec2 p, vec2 lo, vec2 hi)
{
    if (any(lessThan(p, lo)) || any(greaterThan(p, hi))) {
        return 0.0;
    }
    return texture2D(tex, p).a;
}

// Share of this fragment's pixel inside clipBox (box filter, exact for the
// axis-aligned rect).
float clipCoverage(void)
{
    vec2 lo = max(vertex - vec2(0.5), clipBox.xy);
    vec2 hi = min(vertex + vec2(0.5), clipBox.zw);
    vec2 overlap = clamp(hi - lo, 0.0, 1.0);
    return overlap.x * overlap.y;
}

// A rim pixel the client covers only partly (share f) with paint of alpha
// alpha_L has alpha a = f * alpha_L. Seen through, it should show the blur
// behind its painted share and the sharp backdrop elsewhere:
//   f * (paint + (1 - alpha_L) * blur) + (1 - f) * sharp.
// The compositor lays the paint over coverage * blur + (1 - coverage) *
// sharp, so the coverage that matches is (f - a) / (1 - a), not f: with a
// dark ring (alpha_L 0.79) half covering the pixel, 0.17 rather than 0.5,
// whose excess blur read as a faint line along the edge.
float compositeCoverage(float f, float a)
{
    if (a >= 0.999) {
        return f;
    }
    return clamp((f - a) / (1.0 - a), 0.0, 1.0);
}

float rimCoverage(sampler2D tex, vec2 t, float a, vec2 d, vec2 lo, vec2 hi)
{
    float threshold = max(alphaThreshold, 0.0001);
    if (a <= 0.0) {
        return 0.0;
    }
    float minA = a;
    float peak = a;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float n = rimTap(tex, t + vec2(float(x), float(y)) * d, lo, hi);
            minA = min(minA, n);
            peak = max(peak, n);
        }
    }
    if (minA >= alphaFloor && a >= threshold - 2.0 / 255.0) {
        return min(clamp(a / threshold, 0.0, 1.0), clipCoverage());
    }
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            if (abs(float(x)) < 2.0 && abs(float(y)) < 2.0) {
                continue;
            }
            float n = rimTap(tex, t + vec2(float(x), float(y)) * d, lo, hi);
            minA = min(minA, n);
            peak = max(peak, n);
        }
    }
    if (minA >= alphaFloor) {
        return min(clamp(a / threshold, 0.0, 1.0), clipCoverage());
    }
    float f = min(clamp(a / max(threshold, peak), 0.0, 1.0), clipCoverage());
    // A rim whose densest paint is no denser than the fill (a soft shadow's
    // tail) keeps alpha / T; an outline or fill edge is composited exactly.
    return peak >= threshold - 2.0 / 255.0 ? compositeCoverage(f, a) : f;
}

// Coverage contributed by one surface at this fragment.
float surfaceCoverage(sampler2D tex, int i)
{
    vec2 s = (vertex - surfaceBox[i].xy) / max(surfaceBox[i].zw, vec2(0.000001));
    // Outside this surface there is no client pixel to consult.
    if (any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0)))) {
        return 0.0;
    }
    vec2 t = bufferSource[i].xy + s * bufferSource[i].zw;
    t = (alphaTexMatrix[i] * vec4(t, 0.0, 1.0)).xy;
    float a = texture2D(tex, t).a;
    if (alphaMode == 0) {
        return clamp(a / max(alphaThreshold, 0.0001), 0.0, 1.0);
    }
    if (alphaMode == 2) {
        vec2 e0 = (alphaTexMatrix[i] * vec4(bufferSource[i].xy, 0.0, 1.0)).xy;
        vec2 e1 = (alphaTexMatrix[i] * vec4(bufferSource[i].xy + bufferSource[i].zw, 0.0, 1.0)).xy;
        return rimCoverage(tex, t, a, texelSize[i], min(e0, e1), max(e0, e1));
    }

    if (a < alphaFloor) {
        return 0.0;
    }
    // 8-neighbour min/max one texel out. The kernel is symmetric, so applying
    // it after the texture matrix (which may flip) is fine.
    vec2 d = texelSize[i];
    float lo = a;
    float hi = a;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) {
                continue;
            }
            float n = texture2D(tex, t + vec2(float(x), float(y)) * d).a;
            lo = min(lo, n);
            hi = max(hi, n);
        }
    }
    if (lo >= alphaFloor) {
        // Interior of a fill (or a gradient): the fill's own alpha is a colour
        // choice, not a request for weaker blur.
        return 1.0;
    }
    // Edge against transparency: recover the rasteriser's coverage.
    return clamp(a / max(hi, 0.0001), 0.0, 1.0);
}

void main(void)
{
    float coverage = surfaceCoverage(alphaTex0, 0);
    if (alphaCount > 1) {
        coverage = max(coverage, surfaceCoverage(alphaTex1, 1));
    }
    if (alphaCount > 2) {
        coverage = max(coverage, surfaceCoverage(alphaTex2, 2));
    }
    if (alphaCount > 3) {
        coverage = max(coverage, surfaceCoverage(alphaTex3, 3));
    }
    gl_FragColor = texture2D(texUnit, uv) * coverage * modulation;
}
