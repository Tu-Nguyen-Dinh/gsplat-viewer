#pragma once
// DEFLATE (RFC 1951) and gzip (RFC 1952) decompression, written from the RFCs so the project
// needs no zlib. Used by the .spz reader (versions 2 and 3 are one gzip stream).
//
// The decoder follows the structure of zlib's "puff" reference: canonical Huffman codes decoded
// one bit at a time from a count table, which is short and easy to verify, plus a 9-bit lookup
// table for the common short codes (most symbols) so it is not painfully slow.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gsv {

// Raw DEFLATE stream -> bytes. `consumed` (optional) receives the number of input bytes used.
bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error,
                size_t* consumed = nullptr);

// gzip file -> bytes. Checks the header, the CRC-32 and the length stored in the trailer.
bool gunzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error);

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);

inline bool isGzip(const uint8_t* data, size_t size) { return size >= 2 && data[0] == 0x1F && data[1] == 0x8B; }

}  // namespace gsv
