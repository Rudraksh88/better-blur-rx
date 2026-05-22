#version 140

uniform sampler2D texUnitOld;
uniform sampler2D texUnitNew;

in vec2 uv;

out vec4 fragColor;

void main() {
    vec4 colorOld = texture(texUnitOld, uv);
    vec4 colorNew = texture(texUnitNew, uv);

    // discard (almost) transparent pixels
    if (any(lessThan(vec2(colorOld.a, colorNew.a), vec2(0.01)))) {
        discard;
    }

    // discard (almost) identical pixels (using squared vec distance)
    vec4 colorDiff = colorOld - colorNew;
    if (dot(colorDiff, colorDiff) < 0.001) {
        discard;
    }

    // not discarded -> different
    fragColor = vec4(1.0);
}
