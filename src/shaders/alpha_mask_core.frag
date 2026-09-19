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
uniform int alphaCount;
uniform float alphaThreshold;
uniform float modulation;

in vec2 uv;
in vec2 vertex;

out vec4 fragColor;

float surfaceAlpha(sampler2D tex, int i)
{
    vec2 s = (vertex - surfaceBox[i].xy) / max(surfaceBox[i].zw, vec2(0.000001));
    // Outside this surface there is no client pixel to consult.
    if (any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0)))) {
        return 0.0;
    }
    vec2 t = bufferSource[i].xy + s * bufferSource[i].zw;
    t = (alphaTexMatrix[i] * vec4(t, 0.0, 1.0)).xy;
    return texture(tex, t).a;
}

void main(void)
{
    float a = surfaceAlpha(alphaTex0, 0);
    if (alphaCount > 1) {
        a = max(a, surfaceAlpha(alphaTex1, 1));
    }
    if (alphaCount > 2) {
        a = max(a, surfaceAlpha(alphaTex2, 2));
    }
    if (alphaCount > 3) {
        a = max(a, surfaceAlpha(alphaTex3, 3));
    }
    float coverage = clamp(a / max(alphaThreshold, 0.0001), 0.0, 1.0);
    fragColor = texture(texUnit, uv) * coverage * modulation;
}
