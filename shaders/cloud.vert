#version 430 core
// Core 1: opaque point clouds (LiDAR, SfM). Drawn before the splats with depth writes, so they
// occlude splats that test against the depth buffer.

layout(location = 0) in vec3 aPosition;   // offset from the cloud origin (layer-local)
layout(location = 1) in vec4 aColor;      // normalized u8: rgb + unused
layout(location = 2) in vec2 aAttrib;     // x = intensity / 65535, y = ASPRS class

uniform mat4 uModelView;     // layer-local -> camera, built relative to the render origin (double on CPU)
uniform mat4 uProjection;
uniform float uPointSize;    // pixels (uWorldSize <= 0) or ...
uniform float uWorldSize;    // ... metres: projected size = uWorldSize * focal / depth (clamped)
uniform float uFocal;        // focal length in pixels
uniform int uColorMode;      // 0 = rgb, 1 = intensity, 2 = class, 3 = height
uniform vec2 uHeightRange;   // (min, 1 / (max - min)) of the layer-local z or y for the height ramp
uniform int uHeightAxis;     // 1 = y up, 2 = z up

out vec3 vColor;
out float vViewDepth;

vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

vec3 classColor(int c) {
    if (c == 2) return vec3(0.65, 0.50, 0.30);   // ground
    if (c == 3) return vec3(0.55, 0.85, 0.40);   // low vegetation
    if (c == 4) return vec3(0.30, 0.70, 0.25);   // medium vegetation
    if (c == 5) return vec3(0.10, 0.50, 0.10);   // high vegetation
    if (c == 6) return vec3(0.85, 0.25, 0.20);   // building
    if (c == 7) return vec3(1.00, 0.00, 1.00);   // noise
    if (c == 9) return vec3(0.20, 0.40, 0.90);   // water
    return vec3(0.6);
}

void main() {
    vec4 viewPos = uModelView * vec4(aPosition, 1.0);
    gl_Position = uProjection * viewPos;
    vViewDepth = -viewPos.z;
    float size = uPointSize;
    if (uWorldSize > 0.0) size = clamp(uWorldSize * uFocal / max(vViewDepth, 1e-4), 1.0, 64.0);
    gl_PointSize = size;
    if (uColorMode == 1) vColor = vec3(aAttrib.x);
    else if (uColorMode == 2) vColor = classColor(int(aAttrib.y + 0.5));
    else if (uColorMode == 3) vColor = heat((aPosition[uHeightAxis] - uHeightRange.x) * uHeightRange.y);
    else vColor = aColor.rgb;
}
