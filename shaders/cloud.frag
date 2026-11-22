#version 430 core

in vec3 vColor;
in float vViewDepth;

uniform int uRoundPoints;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragDepth;   // opaque: expected depth z, coverage 1 (see splat.frag)

void main() {
    if (uRoundPoints != 0) {
        vec2 c = gl_PointCoord * 2.0 - 1.0;
        if (dot(c, c) > 1.0) discard;
    }
    fragColor = vec4(vColor, 1.0);
    fragDepth = vec4(vViewDepth, 1.0, 0.0, 1.0);
}
