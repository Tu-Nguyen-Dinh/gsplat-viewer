// Per-Gaussian math shared by the compute shaders.
// Mirrors src/core/gaussian_math.cpp line by line; tests compare both.

const float kScreenDilation = 0.3;  // pixels^2 added to the 2D covariance (anti-aliasing)
const float kSigmaExtent = 3.0;     // draw out to 3 standard deviations

// Unpacks the 6 unique entries of a symmetric 3x3 covariance.
mat3 unpackCovariance(vec4 c0, vec4 c1) {
    // c0 = (xx, xy, xz, yy), c1 = (yz, zz, -, -)
    return mat3(c0.x, c0.y, c0.z,
                c0.y, c0.w, c1.x,
                c0.z, c1.x, c1.y);
}

struct Projection {
    bool visible;
    vec2 center;      // pixels, origin bottom-left
    float depth;      // distance along the view direction
    vec3 conic;       // inverse 2D covariance (a, b, c)
    vec2 axisMajor;   // 3-sigma half axes in pixels
    vec2 axisMinor;
    float radius;
};

// EWA splatting: Sigma' = J W Sigma W^T J^T  (see gaussian_math.cpp for the derivation).
Projection projectGaussian(vec3 mean, mat3 cov, mat4 view, vec2 viewport, vec2 focal, vec2 tanHalfFov,
                           float nearPlane, float splatScale, bool cullOutsideViewport) {
    Projection o;
    o.visible = false;

    vec3 t = (view * vec4(mean, 1.0)).xyz;
    float z = -t.z;
    if (z < nearPlane) return o;

    vec2 lim = 1.3 * tanHalfFov;
    float tx = clamp(t.x / z, -lim.x, lim.x) * z;
    float ty = clamp(t.y / z, -lim.y, lim.y) * z;
    float j00 = focal.x / z, j02 = focal.x * tx / (z * z);
    float j11 = focal.y / z, j12 = focal.y * ty / (z * z);

    mat3 W = mat3(view);
    mat3 S = W * (cov * (splatScale * splatScale)) * transpose(W);

    float a = j00 * j00 * S[0][0] + 2.0 * j00 * j02 * S[2][0] + j02 * j02 * S[2][2];
    float b = j00 * j11 * S[1][0] + j00 * j12 * S[2][0] + j02 * j11 * S[2][1] + j02 * j12 * S[2][2];
    float c = j11 * j11 * S[1][1] + 2.0 * j11 * j12 * S[2][1] + j12 * j12 * S[2][2];
    a += kScreenDilation;
    c += kScreenDilation;

    float det = a * c - b * b;
    if (det <= 0.0) return o;

    float mid = 0.5 * (a + c);
    float disc = sqrt(max(0.1, mid * mid - det));
    float lambda1 = mid + disc;
    float lambda2 = max(mid - disc, 0.1);
    vec2 v1 = abs(b) > 1e-6 ? normalize(vec2(b, lambda1 - a)) : (a >= c ? vec2(1, 0) : vec2(0, 1));
    vec2 v2 = vec2(-v1.y, v1.x);

    o.center = vec2(focal.x * t.x / z, focal.y * t.y / z) + 0.5 * viewport;
    o.depth = z;
    o.conic = vec3(c, -b, a) / det;
    o.radius = ceil(kSigmaExtent * sqrt(lambda1));
    o.axisMajor = v1 * (kSigmaExtent * sqrt(lambda1));
    o.axisMinor = v2 * (kSigmaExtent * sqrt(lambda2));

    if (cullOutsideViewport) {
        float r = o.radius;
        if (o.center.x + r < 0.0 || o.center.x - r > viewport.x ||
            o.center.y + r < 0.0 || o.center.y - r > viewport.y) return o;
    }
    o.visible = true;
    return o;
}
