// View-dependent color from spherical harmonics (degree 0..3).
// Mirrors evalSphericalHarmonics() in src/core/gaussian_math.cpp.
//
// Requires, before inclusion:
//   buffer ... { uint shData[]; }   higher-band coefficients of every splat, PLY order
//                                    (channel-major: R[n], G[n], B[n]) stored either as
//                                    float32 bits or as packed pairs of float16

uniform int uShHalf;   // 1 = float16 pairs, 0 = float32

const float SH_C0 = 0.28209479177387814;
const float SH_C1 = 0.4886025119029199;
const float SH_C2[5] = float[5](1.0925484305920792, -1.0925484305920792, 0.31539156525252005,
                                -1.0925484305920792, 0.5462742152960396);
const float SH_C3[7] = float[7](-0.5900435899266435, 2.890611442640554, -0.4570457994644658,
                                0.3731763325901154, -0.4570457994644658, 1.445305721320277,
                                -0.5900435899266435);

// Value number `idx` of the coefficient block starting at uint offset `base`.
float shValue(uint base, uint idx) {
    if (uShHalf != 0) {
        vec2 pair = unpackHalf2x16(shData[base + (idx >> 1u)]);   // two halves per uint
        return (idx & 1u) == 0u ? pair.x : pair.y;
    }
    return uintBitsToFloat(shData[base + idx]);
}

// Coefficient k (k >= 1) for the three channels.
vec3 shCoeff(uint base, uint perChannel, uint k) {
    return vec3(shValue(base, k - 1u),
                shValue(base, perChannel + k - 1u),
                shValue(base, 2u * perChannel + k - 1u));
}

vec3 evalSphericalHarmonics(int degree, vec3 dir, vec3 dc, uint base, uint perChannel) {
    vec3 result = SH_C0 * dc;
    if (degree > 0 && perChannel >= 3u) {
        float x = dir.x, y = dir.y, z = dir.z;
        result += -SH_C1 * y * shCoeff(base, perChannel, 1u)
                 + SH_C1 * z * shCoeff(base, perChannel, 2u)
                 - SH_C1 * x * shCoeff(base, perChannel, 3u);
        if (degree > 1 && perChannel >= 8u) {
            float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, xz = x * z;
            result += SH_C2[0] * xy * shCoeff(base, perChannel, 4u)
                    + SH_C2[1] * yz * shCoeff(base, perChannel, 5u)
                    + SH_C2[2] * (2.0 * zz - xx - yy) * shCoeff(base, perChannel, 6u)
                    + SH_C2[3] * xz * shCoeff(base, perChannel, 7u)
                    + SH_C2[4] * (xx - yy) * shCoeff(base, perChannel, 8u);
            if (degree > 2 && perChannel >= 15u) {
                result += SH_C3[0] * y * (3.0 * xx - yy) * shCoeff(base, perChannel, 9u)
                        + SH_C3[1] * xy * z * shCoeff(base, perChannel, 10u)
                        + SH_C3[2] * y * (4.0 * zz - xx - yy) * shCoeff(base, perChannel, 11u)
                        + SH_C3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy) * shCoeff(base, perChannel, 12u)
                        + SH_C3[4] * x * (4.0 * zz - xx - yy) * shCoeff(base, perChannel, 13u)
                        + SH_C3[5] * z * (xx - yy) * shCoeff(base, perChannel, 14u)
                        + SH_C3[6] * x * (xx - 3.0 * yy) * shCoeff(base, perChannel, 15u);
            }
        }
    }
    return max(result + 0.5, vec3(0.0));
}
