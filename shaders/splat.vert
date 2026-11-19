#version 430 core
// Pass 3: one instanced quad per visible splat, drawn in the order given by `order[]`
// (back-to-front after sorting). Four vertices per instance form a triangle strip.
#include "include/splat_buffers.glsl"

layout(std430, binding = BINDING_PROJECTED) readonly buffer Projected { ProjectedSplat projected[]; };
layout(std430, binding = BINDING_ORDER)     readonly buffer Order     { uint order[]; };

uniform vec2 uViewport;
uniform int uOrientedQuads;   // 1: rectangle aligned with the ellipse axes, 0: square of side 2*radius

out vec2 vOffset;                    // pixel offset from the splat center
flat out vec4 vConicOpacity;
flat out vec4 vColorDepth;           // rgb = color, a = depth (debug views)

void main() {
    ProjectedSplat s = projected[order[gl_InstanceID]];
    if (s.centerDepthRadius.w <= 0.0) {          // culled (only happens with CPU ordering)
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // outside the clip volume: no fragments
        return;
    }

    // gl_VertexID 0..3 -> (-1,-1) (1,-1) (-1,1) (1,1)
    vec2 corner = vec2(gl_VertexID & 1, (gl_VertexID >> 1) & 1) * 2.0 - 1.0;
    vec2 offset = uOrientedQuads != 0 ? corner.x * s.axes.xy + corner.y * s.axes.zw
                                      : corner * s.centerDepthRadius.w;
    vec2 pixel = s.centerDepthRadius.xy + offset;

    gl_Position = vec4(pixel / uViewport * 2.0 - 1.0, 0.0, 1.0);
    vOffset = offset;
    vConicOpacity = s.conicOpacity;
    vColorDepth = vec4(s.color.rgb, s.centerDepthRadius.z);
}
