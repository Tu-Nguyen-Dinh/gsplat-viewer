#version 430 core
// Day 1: one point per Gaussian center.

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;   // rgb = SH degree-0 color, a = opacity

uniform mat4 uViewProj;
uniform mat4 uModel;
uniform float uPointSize;

out vec4 vColor;

void main() {
    gl_Position = uViewProj * uModel * vec4(aPosition, 1.0);
    gl_PointSize = uPointSize;
    vColor = aColor;
}
