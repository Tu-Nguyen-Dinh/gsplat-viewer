// Shader storage buffer layouts shared by the splat shaders and SplatRenderer (C++).
// std430 + vec4-only members keep the CPU and GPU layouts identical without padding rules.

struct GpuSplat {          // uploaded once per scene (activations pre-applied on the CPU)
    vec4 positionOpacity;  // xyz = mean, w = opacity in [0,1]
    vec4 cov0;             // 3D covariance xx, xy, xz, yy
    vec4 cov1;             // yz, zz, -, -
    vec4 dc;               // SH band 0 (rgb), w unused
};

struct ProjectedSplat {    // written by the preprocess pass every frame
    vec4 centerDepthRadius;  // xy = center (px), z = depth, w = radius (0 = culled)
    vec4 conicOpacity;       // xyz = inverse 2D covariance, w = opacity
    vec4 axes;               // xy = major half axis, zw = minor half axis (px)
    vec4 color;              // rgb = SH color for this view
};

// Layout matches the arguments of glDrawArraysIndirect.
struct DrawCommand {
    uint count;          // vertices per instance (4 = quad as a triangle strip)
    uint instanceCount;  // number of visible splats, incremented atomically
    uint first;
    uint baseInstance;
};

#define BINDING_SPLATS     0
#define BINDING_SH_REST    1
#define BINDING_PROJECTED  2
#define BINDING_ORDER      3
#define BINDING_DRAW_CMD   4
#define BINDING_SORT_KEYS  5
