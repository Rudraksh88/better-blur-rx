#version 140

// Final composite of a cached blur, shaped by the window's own alpha channel.
// texUnit holds the blurred background (premultiplied). alphaTex0..3 are the
// window's surface textures: the root surface and up to three subsurfaces
// (GTK4 graphics offload, video players). Coverage is the maximum surface
// alpha, normalised by a threshold so translucent fills still receive full
// blur while the antialiased silhouette the client rasterised itself becomes
// the blur's silhouette.
uniform sampler2D texUnit;
uniform sampler2D alphaTex0;
uniform sampler2D alphaTex1;
uniform sampler2D alphaTex2;
uniform sampler2D alphaTex3;
// Per surface: maps normalised buffer coordinates to texture coordinates
// (y-flip, buffer transform), as KWin's own surface rendering does.
uniform mat4 alphaTexMatrix[4];
// Per surface: rect in device pixels, local to the blur's background rect.
uniform vec4 surfaceBox[4];
// Per surface: wp_viewporter source box normalised to the buffer size.
uniform vec4 bufferSource[4];
// Per surface: one texel in texture coordinates, for the silhouette kernel.
uniform vec2 texelSize[4];
uniform int alphaCount;
// 0 = threshold: coverage = alpha / alphaThreshold, strength follows the fill.
// 1 = silhouette: full coverage inside any fill above alphaFloor; at an edge
//     against transparency coverage = alpha / neighbourhood max, i.e. the
//     client's own antialiasing. Strength never depends on the fill alpha.
uniform int alphaMode;
uniform float alphaThreshold;
uniform float alphaFloor;
uniform float modulation;

in vec2 uv;
in vec2 vertex;

out vec4 fragColor;

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
    float a = texture(tex, t).a;
    if (alphaMode == 0) {
        return clamp(a / max(alphaThreshold, 0.0001), 0.0, 1.0);
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
            float n = texture(tex, t + vec2(float(x), float(y)) * d).a;
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
    fragColor = texture(texUnit, uv) * coverage * modulation;
}
