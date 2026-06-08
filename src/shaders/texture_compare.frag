uniform sampler2D texUnitOld;
uniform sampler2D texUnitNew;

varying vec2 uv;

void main() {
    // get texture coordinate, clamped into texture
    ivec2 texSize = textureSize(texUnitOld, 0);
    ivec2 physCoord = clamp(ivec2(floor(uv * vec2(texSize))), ivec2(0), texSize - ivec2(1));

    // read raw colors
    vec4 oldColor = texelFetch(texUnitOld, physCoord, 0);
    vec4 newColor = texelFetch(texUnitNew, physCoord, 0);

    vec4 diff = abs(oldColor - newColor);
    if (all(lessThan(diff, vec4(0.01)))) {
        discard;
    }

    // write new color back to cached blit
    gl_FragColor = newColor;
}
