// Decoder of the compressed splat format (24 bytes per splat). Must match src/scene/splat_codec.cpp:
//   word 0 : position x (u16) | position y (u16)     relative to the chunk bounds
//   word 1 : position z (u16) | opacity (f16)
//   word 2 : log scale x (f16) | log scale y (f16)
//   word 3 : log scale z (f16) | SH DC red (f16)
//   word 4 : SH DC green (f16) | SH DC blue (f16)
//   word 5 : rotation, smallest-three quaternion (2-bit index of the dropped component + 3 x 10 bits)

const uint kCodecChunk = 256u;

vec4 unpackQuaternion(uint v) {
    uint largest = v >> 30;
    vec4 q = vec4(0.0);
    float sum = 0.0;
    uint shift = 20u;
    for (uint i = 0u; i < 4u; ++i) {
        if (i == largest) continue;
        float c = (float((v >> shift) & 1023u) / 1023.0 * 2.0 - 1.0) * 0.70710678;
        q[i] = c;
        sum += c * c;
        shift -= 10u;
    }
    q[largest] = sqrt(max(0.0, 1.0 - sum));
    return q;
}

struct DecodedSplat {
    vec3 position;
    float opacity;
    vec3 scale;     // exp(log scale)
    vec3 dc;
    vec4 rotation;  // w, x, y, z
};

// words[6 * i .. 6 * i + 5]; chunkBounds[2 * c] = min, [2 * c + 1] = max (xyz).
DecodedSplat decodeSplat(uint w0, uint w1, uint w2, uint w3, uint w4, uint w5, vec3 lo, vec3 hi) {
    DecodedSplat d;
    vec3 t = vec3(float(w0 & 0xFFFFu), float(w0 >> 16), float(w1 & 0xFFFFu)) / 65535.0;
    d.position = lo + (hi - lo) * t;
    d.opacity = unpackHalf2x16(w1).y;
    vec2 sxy = unpackHalf2x16(w2), szr = unpackHalf2x16(w3), dgb = unpackHalf2x16(w4);
    d.scale = exp(vec3(sxy, szr.x));
    d.dc = vec3(szr.y, dgb);
    d.rotation = unpackQuaternion(w5);
    return d;
}
