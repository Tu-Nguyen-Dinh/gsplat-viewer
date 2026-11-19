#version 430 core

in vec4 vColor;
uniform float uMinOpacity;
out vec4 fragColor;

void main() {
    // Round points instead of squares.
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    if (dot(c, c) > 1.0 || vColor.a < uMinOpacity) discard;
    fragColor = vec4(vColor.rgb, 1.0);
}
