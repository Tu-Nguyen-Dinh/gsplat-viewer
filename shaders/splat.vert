#version 430 core
// Pass 3: one instanced quad per visible splat, drawn in the order given by `order[]`
// (back-to-front after sorting). Four vertices per instance form a triangle strip.
#include "include/splat_buffers.glsl"

layout(std430, binding = BINDING_PROJECTED) readonly buffer Projected { ProjectedSplat projected[]; };
layout(std430, binding = BINDING_ORDER)     readonly buffer Order     { uint order[]; };
layout(std430, binding = BINDING_DEPTH_GRAD) readonly buffer DepthGrad { vec2 depthGrad[]; };

#include "include/gaussian.glsl"

uniform vec2 uViewport;
uniform int uOrientedQuads;   // 1: rectangle aligned with the ellipse axes, 0: square of side 2*radius
uniform int uDepthGrad;       // 1: per-pixel depth from the screen-space depth gradient
uniform vec2 uProjZ;          // (P[2][2], P[3][2]) of the projection: NDC z of the splat center, so
                              // opaque layers in the depth buffer can occlude splats (depth test)

out vec2 vOffset;                    // pixel offset from the splat center
flat out vec4 vConicOpacity;
flat out vec4 vColorDepth;           // rgb = color, a = depth (debug views)
flat out vec2 vDepthGrad;            // depth change per pixel of offset (0 unless uDepthGrad)

void main() {
    uint index = order[gl_InstanceID];
    ProjectedSplat s = projected[index];
    if (s.bits.w == 0u) {                        // culled (only happens with CPU ordering)
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // outside the clip volume: no fragments
        return;
    }
    vec3 conic = vec3(s.centerConic.zw, uintBitsToFloat(s.bits.x));
    vec2 rg = unpackHalf2x16(s.bits.z), bo = unpackHalf2x16(s.bits.w);
    float opacity = bo.y;

    // The axes are rebuilt from the conic (4 vertices x a 2x2 eigen decomposition) rather than
    // stored: 16 fewer bytes per splat written and read every frame.
    vec3 cov = covarianceFromConic(conic);
    vec2 axisMajor, axisMinor;
    ellipseAxes(cov.x, cov.y, cov.z, axisMajor, axisMinor);
    float shrink = opacityAwareShrink(opacity);

    // gl_VertexID 0..3 -> (-1,-1) (1,-1) (-1,1) (1,1)
    vec2 corner = vec2(gl_VertexID & 1, (gl_VertexID >> 1) & 1) * 2.0 - 1.0;
    vec2 offset = uOrientedQuads != 0 ? (corner.x * axisMajor + corner.y * axisMinor) * shrink
                                      : corner * ceil(length(axisMajor) * shrink);
    vec2 pixel = s.centerConic.xy + offset;

    float depth = uintBitsToFloat(s.bits.y);
    // Perspective: clip.z = P22 * z_view + P32 with z_view = -depth, clip.w = depth. Clamped so far
    // splats are never clipped (they were drawn at z = 0 before depth testing existed).
    float ndcZ = clamp((uProjZ.y - uProjZ.x * depth) / max(depth, 1e-6), -1.0, 1.0);
    gl_Position = vec4(pixel / uViewport * 2.0 - 1.0, ndcZ, 1.0);
    vOffset = offset;
    vConicOpacity = vec4(conic, opacity);
    vColorDepth = vec4(rg, bo.x, uintBitsToFloat(s.bits.y));
    vDepthGrad = uDepthGrad != 0 ? depthGrad[index] : vec2(0.0);
}
