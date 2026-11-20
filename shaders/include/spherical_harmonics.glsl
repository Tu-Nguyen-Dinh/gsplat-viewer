// View-dependent color from spherical harmonics (degree 0..3).
// Mirrors evalSphericalHarmonics() in src/core/gaussian_math.cpp.
//
// Requires, before inclusion:
//   buffer ... { uint shData[]; }   higher-band coefficients, stored COEFFICIENT-MAJOR:
//                                    word w of splat s is shData[w * uShCount + s] (coalesced
//                                    loads: neighbouring threads read neighbouring words).
//                                    Value v (PLY order, channel-major R[n], G[n], B[n]) is word
//                                    v (float32 bits) or half (v & 1) of word v / 2 (float16)

uniform int uShHalf;    // 1 = float16 pairs, 0 = float32
uniform uint uShCount;  // number of splats (the stride between coefficient planes)

const float SH_C0 = 0.28209479177387814;
const float SH_C1 = 0.4886025119029199;
const float SH_C2[5] = float[5](1.0925484305920792, -1.0925484305920792, 0.31539156525252005,
                                -1.0925484305920792, 0.5462742152960396);
const float SH_C3[7] = float[7](-0.5900435899266435, 2.890611442640554, -0.4570457994644658,
                                0.3731763325901154, -0.4570457994644658, 1.445305721320277,
                                -0.5900435899266435);

// Value number `idx` of splat `splat`.
float shValue(uint splat, uint idx) {
    if (uShHalf != 0) {
        vec2 pair = unpackHalf2x16(shData[(idx >> 1u) * uShCount + splat]);   // two halves per uint
        return (idx & 1u) == 0u ? pair.x : pair.y;
    }
    return uintBitsToFloat(shData[idx * uShCount + splat]);
}

// Coefficient k (k >= 1) for the three channels.
vec3 shCoeff(uint splat, uint perChannel, uint k) {
    return vec3(shValue(splat, k - 1u),
                shValue(splat, perChannel + k - 1u),
                shValue(splat, 2u * perChannel + k - 1u));
}

vec3 evalSphericalHarmonics(int degree, vec3 dir, vec3 dc, uint splat, uint perChannel) {
    vec3 result = SH_C0 * dc;
    if (degree > 0 && perChannel >= 3u) {
        float x = dir.x, y = dir.y, z = dir.z;
        result += -SH_C1 * y * shCoeff(splat, perChannel, 1u)
                 + SH_C1 * z * shCoeff(splat, perChannel, 2u)
                 - SH_C1 * x * shCoeff(splat, perChannel, 3u);
        if (degree > 1 && perChannel >= 8u) {
            float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, xz = x * z;
            result += SH_C2[0] * xy * shCoeff(splat, perChannel, 4u)
                    + SH_C2[1] * yz * shCoeff(splat, perChannel, 5u)
                    + SH_C2[2] * (2.0 * zz - xx - yy) * shCoeff(splat, perChannel, 6u)
                    + SH_C2[3] * xz * shCoeff(splat, perChannel, 7u)
                    + SH_C2[4] * (xx - yy) * shCoeff(splat, perChannel, 8u);
            if (degree > 2 && perChannel >= 15u) {
                result += SH_C3[0] * y * (3.0 * xx - yy) * shCoeff(splat, perChannel, 9u)
                        + SH_C3[1] * xy * z * shCoeff(splat, perChannel, 10u)
                        + SH_C3[2] * y * (4.0 * zz - xx - yy) * shCoeff(splat, perChannel, 11u)
                        + SH_C3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy) * shCoeff(splat, perChannel, 12u)
                        + SH_C3[4] * x * (4.0 * zz - xx - yy) * shCoeff(splat, perChannel, 13u)
                        + SH_C3[5] * z * (xx - yy) * shCoeff(splat, perChannel, 14u)
                        + SH_C3[6] * x * (xx - 3.0 * yy) * shCoeff(splat, perChannel, 15u);
            }
        }
    }
    return max(result + 0.5, vec3(0.0));
}
