#pragma once
// Reader for Niantic's .spz format, versions 2 to 4.
//   v2, v3: the whole file is one gzip stream (decoded by core/inflate, written for this project)
//   v4:     uncompressed header + table, one zstd frame per attribute (third_party/zstd)
//
// Decompressed: a 16-byte header, then each attribute for all points in turn:
//   positions  3 x 24-bit signed fixed point (fractionalBits from the header)
//   alphas     u8, opacity = a / 255
//   colors     3 x u8, SH DC = (c / 255 - 0.5) / 0.15
//   scales     3 x u8, log scale = s / 16 - 10
//   rotations  v2: 3 x u8 (x, y, z; w >= 0 implied)  v3, v4: 32-bit smallest-three quaternion
//   SH         (degree 1/2/3: 3/8/15 coefficients) x 3 channels, u8, value = (v - 128) / 128

#include "scene/gaussian_scene.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gsv {

bool loadSpz(const std::filesystem::path& path, GaussianScene& out, std::string& error);
// Decodes an already decompressed .spz payload (header + attributes). Exposed for tests.
bool decodeSpzPayload(const std::vector<uint8_t>& payload, GaussianScene& out, std::string& error);

}  // namespace gsv
