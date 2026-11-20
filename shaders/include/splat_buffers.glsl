// Shader storage buffer layouts shared by the splat shaders and SplatRenderer (C++).
// std430 + vec4/uvec4-only members keep the CPU and GPU layouts identical without padding rules.

struct GpuSplat {          // full-precision format (--no-compress): activations pre-applied on the CPU
    vec4 positionOpacity;  // xyz = mean, w = opacity in [0,1]
    vec4 cov0;             // 3D covariance xx, xy, xz, yy
    vec4 cov1;             // yz, zz, -, -
    vec4 dc;               // SH band 0 (rgb), w unused
};
// The compressed format (default) is a plain uint array, 6 words per splat: see splat_codec.glsl.

struct ProjectedSplat {    // written by the preprocess pass every frame, 32 bytes
    vec4 centerConic;      // xy = center (px), zw = conic xx, xy
    uvec4 bits;            // x = conic yy (float bits), y = depth (float bits),
                           // z = half2(red, green), w = half2(blue, opacity); w == 0 -> culled
};
// The ellipse axes are not stored: the vertex shader rebuilds them from the conic.
// ("packed" would be a nicer name, but it is a reserved word in GLSL.)

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
#define BINDING_PACKED     6
#define BINDING_CHUNKS     7
