#version 430 core
// Evaluates the 2D Gaussian at this pixel and outputs a premultiplied color.
// Blending (ONE, ONE_MINUS_SRC_ALPHA) composites the splats back-to-front:
//   C = alpha * c_splat + (1 - alpha) * C_behind

in vec2 vOffset;
flat in vec4 vConicOpacity;
flat in vec4 vColorDepth;

uniform int uDebugView;      // 0 = color, 1 = depth, 2 = overdraw, 3 = ellipse outlines
uniform vec2 uDepthRange;    // (min, 1 / (max - min)) for the depth view
uniform int uCountCoverage;  // benchmark: keep every rasterized fragment so a query can count them

out vec4 fragColor;

// Compact "turbo"-like colormap for debug views.
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

void main() {
    if (uCountCoverage != 0) {        // counted by a GL_SAMPLES_PASSED query, contributes nothing
        fragColor = vec4(0.0);
        return;
    }
    vec3 c = vConicOpacity.xyz;
    vec2 d = vOffset;
    float power = -0.5 * (c.x * d.x * d.x + 2.0 * c.y * d.x * d.y + c.z * d.y * d.y);
    if (power > 0.0) discard;
    float alpha = min(0.99, vConicOpacity.w * exp(power));
    if (alpha < 1.0 / 255.0) discard;   // invisible contribution: skip the blend

    vec3 color = min(vColorDepth.rgb, vec3(1.0));   // SH can exceed 1; same clamp as the CPU reference
    if (uDebugView == 1) {
        color = heat(1.0 - (vColorDepth.a - uDepthRange.x) * uDepthRange.y);
    } else if (uDebugView == 2) {
        // Additive: each layer adds a little. Red saturates after ~80 layers,
        // green after ~200, blue after ~500, giving a black-red-yellow-white heat scale.
        fragColor = vec4(0.012, 0.005, 0.002, 0.0);
        return;
    } else if (uDebugView == 3) {
        // power = -4.5 at 3 sigma; keep a thin ring around 2 sigma (power = -2)
        if (abs(power + 2.0) > 0.12) discard;
        fragColor = vec4(color, 1.0);
        return;
    }
    fragColor = vec4(color * alpha, alpha);
}
