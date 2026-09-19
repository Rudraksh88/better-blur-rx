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
uniform int alphaCount;
uniform float alphaThreshold;
uniform float modulation;

varying vec2 uv;
varying vec2 vertex;

float surfaceAlpha(sampler2D tex, int i)
{
    vec2 s = (vertex - surfaceBox[i].xy) / max(surfaceBox[i].zw, vec2(0.000001));
    if (any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0)))) {
        return 0.0;
    }
    vec2 t = bufferSource[i].xy + s * bufferSource[i].zw;
    t = (alphaTexMatrix[i] * vec4(t, 0.0, 1.0)).xy;
    return texture2D(tex, t).a;
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
    gl_FragColor = texture2D(texUnit, uv) * coverage * modulation;
}
