// Per-Gaussian math shared by the splat shaders.
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

// Unit quaternion (w, x, y, z) -> rotation matrix (same as rotationFromQuaternion in C++).
mat3 rotationFromQuaternion(vec4 q) {
    float w = q.x, x = q.y, y = q.z, z = q.w;
    return mat3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y),   // column 0
                2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x),   // column 1
                2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y));  // column 2
}

// Sigma = R S S^T R^T, built from the per-axis scale and the rotation.
mat3 covarianceFromScaleRotation(vec3 scale, vec4 q) {
    mat3 R = rotationFromQuaternion(q);
    mat3 M = mat3(R[0] * scale.x, R[1] * scale.y, R[2] * scale.z);
    return M * transpose(M);
}

// 3-sigma half axes (pixels) of the ellipse with 2D covariance [[a, b], [b, c]]: the
// eigenvectors scaled by 3 * sqrt(eigenvalue). Used by the preprocess pass (culling radius) and
// by the vertex shader, which rebuilds the axes from the conic instead of storing them.
void ellipseAxes(float a, float b, float c, out vec2 axisMajor, out vec2 axisMinor) {
    float mid = 0.5 * (a + c);
    float disc = sqrt(max(0.1, mid * mid - (a * c - b * b)));
    float lambda1 = mid + disc;
    float lambda2 = max(mid - disc, 0.1);
    vec2 v1 = abs(b) > 1e-6 ? normalize(vec2(b, lambda1 - a)) : (a >= c ? vec2(1, 0) : vec2(0, 1));
    vec2 v2 = vec2(-v1.y, v1.x);
    axisMajor = v1 * (kSigmaExtent * sqrt(lambda1));
    axisMinor = v2 * (kSigmaExtent * sqrt(lambda2));
}

// Inverse of the conic (inverse 2D covariance) stored per projected splat: back to (a, b, c).
vec3 covarianceFromConic(vec3 conic) {
    float d = conic.x * conic.z - conic.y * conic.y;
    return vec3(conic.z, -conic.y, conic.x) / d;
}

// The fragment shader discards alpha < 1/255, i.e. beyond sigma * sqrt(2 ln(255 opacity)) (<= 3
// sigma). A faint splat (opacity 0.05: 1.7 sigma) would otherwise rasterize a 3-sigma quad of
// which ~70 % is discarded. Same image, fewer fragments.
float opacityAwareShrink(float opacity) {
    return min(kSigmaExtent, sqrt(2.0 * log(max(255.0 * opacity, 1.0)))) / kSigmaExtent;
}

struct Projection {
    bool visible;
    vec2 center;      // pixels, origin bottom-left
    float depth;      // distance along the view direction
    vec3 conic;       // inverse 2D covariance (a, b, c)
    vec2 axisMajor;   // 3-sigma half axes in pixels
    vec2 axisMinor;
    float radius;
    vec2 depthGradient; // d(depth)/d(pixel): conditional mean of the depth given the pixel (Core 1)
};

// EWA splatting: Sigma' = J W Sigma W^T J^T  (see gaussian_math.cpp for the derivation).
Projection projectGaussian(vec3 mean, mat3 cov, mat4 view, vec2 viewport, vec2 focal, vec2 principal, vec2 tanHalfFov,
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
    // Inverse of the undilated 2D covariance, for the depth gradient below (dilation is an
    // anti-aliasing blur of the footprint, not part of the surface's geometry).
    float det0 = a * c - b * b;
    vec3 conic0 = det0 > 1e-12 ? vec3(c, -b, a) / det0 : vec3(0.0);
    a += kScreenDilation;
    c += kScreenDilation;

    float det = a * c - b * b;
    if (det <= 0.0) return o;

    o.center = vec2(focal.x * t.x / z, focal.y * t.y / z) + principal;
    o.depth = z;
    o.conic = vec3(c, -b, a) / det;
    // Depth along a pixel ray through a 3D Gaussian is (in the linearized projection) Gaussian too;
    // its mean at screen offset du is z + g.du with g = Sigma_uu^-1 Cov(z, u), Cov(z, u) = -e_z^T S J^T.
    vec2 czu = -vec2(j00 * S[2][0] + j02 * S[2][2], j11 * S[2][1] + j12 * S[2][2]);
    o.depthGradient = vec2(conic0.x * czu.x + conic0.y * czu.y, conic0.y * czu.x + conic0.z * czu.y);
    ellipseAxes(a, b, c, o.axisMajor, o.axisMinor);
    o.radius = ceil(length(o.axisMajor));

    if (cullOutsideViewport) {
        float r = o.radius;
        if (o.center.x + r < 0.0 || o.center.x - r > viewport.x ||
            o.center.y + r < 0.0 || o.center.y - r > viewport.y) return o;
    }
    o.visible = true;
    return o;
}
