#include "core/inflate.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace gsv {

namespace {

constexpr int kMaxBits = 15;   // longest DEFLATE code
constexpr int kFastBits = 9;   // codes up to 9 bits are decoded with one table lookup

// Reads bits LSB-first, as DEFLATE packs them. Reading past the end yields zeros; the caller
// checks pastEnd() and reports a truncated stream.
struct BitReader {
  const uint8_t* data;
  size_t size, pos = 0;
  uint64_t buffer = 0;
  int count = 0;

  void refill() {
    while (count <= 56) {
      uint64_t byte = 0;
      if (pos < size) byte = data[pos];  // look-ahead past the end reads zeros
      ++pos;
      buffer |= byte << count;
      count += 8;
    }
  }
  uint32_t peek(int n) {
    if (count < n) refill();
    return static_cast<uint32_t>(buffer & ((1ull << n) - 1));
  }
  void skip(int n) {
    buffer >>= n;
    count -= n;
  }
  uint32_t bits(int n) {
    if (n == 0) return 0;
    const uint32_t v = peek(n);
    skip(n);
    return v;
  }
  void alignToByte() { skip(count % 8); }
  size_t bytesConsumed() const { return pos - static_cast<size_t>(count / 8); }
  bool pastEnd() const { return bytesConsumed() > size; }
};

// Canonical Huffman code: count[len] codes of each length, symbols sorted by (length, value).
struct Huffman {
  std::array<uint16_t, kMaxBits + 1> count{};
  std::array<uint16_t, 288> symbol{};
  std::array<uint16_t, 1 << kFastBits> fast{};  // (symbol << 4) | length, 0 = not a short code

  // Returns false for an over-subscribed (invalid) code. Incomplete codes are allowed (a single
  // distance code is legal), decoding a missing code then fails in decode().
  bool build(const uint8_t* lengths, int n) {
    count.fill(0);
    for (int s = 0; s < n; ++s) ++count[lengths[s]];
    count[0] = 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
      left = (left << 1) - count[len];
      if (left < 0) return false;
    }
    std::array<uint16_t, kMaxBits + 2> offset{};
    for (int len = 1; len <= kMaxBits; ++len) offset[len + 1] = offset[len] + count[len];
    for (int s = 0; s < n; ++s)
      if (lengths[s]) symbol[offset[lengths[s]]++] = static_cast<uint16_t>(s);

    // Fast table: canonical code of each short symbol, bit-reversed (the stream is LSB-first),
    // repeated for every value of the bits that follow it.
    fast.fill(0);
    uint32_t code = 0;
    int index = 0;
    for (int len = 1; len <= kFastBits; ++len) {
      for (int k = 0; k < count[len]; ++k, ++code, ++index) {
        uint32_t reversed = 0;
        for (int b = 0; b < len; ++b) reversed |= ((code >> b) & 1u) << (len - 1 - b);
        for (uint32_t fill = 0; fill < (1u << (kFastBits - len)); ++fill)
          fast[reversed | (fill << len)] = static_cast<uint16_t>(symbol[index] << 4 | len);
      }
      code <<= 1;
    }
    return true;
  }

  // Symbol of the next code, or -1 if the bits match no code.
  int decode(BitReader& in) const {
    const uint16_t f = fast[in.peek(kFastBits)];
    if (f) {
      in.skip(f & 15);
      return f >> 4;
    }
    // Long code: walk the lengths one bit at a time (zlib's puff.c). For each length, the codes
    // of that length are the integers [first, first + count) when read MSB-first.
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
      code |= static_cast<int>(in.bits(1));
      const int c = count[len];
      if (code - c < first) return symbol[index + (code - first)];
      index += c;
      first = (first + c) << 1;
      code <<= 1;
    }
    return -1;
  }
};

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Decodes literal/length + distance symbols until end-of-block (256).
bool decodeBlock(BitReader& in, const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out,
                 std::string& error) {
  for (;;) {
    const int sym = lit.decode(in);
    if (sym < 0 || in.pastEnd()) return error = in.pastEnd() ? "truncated stream" : "invalid literal/length code", false;
    if (sym < 256) {
      out.push_back(static_cast<uint8_t>(sym));
    } else if (sym == 256) {
      return true;
    } else {
      const int li = sym - 257;
      if (li >= 29) return error = "invalid length symbol", false;
      const size_t length = kLengthBase[li] + in.bits(kLengthExtra[li]);
      const int ds = dist.decode(in);
      if (ds < 0 || ds >= 30) return error = "invalid distance code", false;
      const size_t distance = kDistBase[ds] + in.bits(kDistExtra[ds]);
      if (distance > out.size()) return error = "distance too far back", false;
      // Byte by byte: the copy may overlap its own output (distance < length repeats a pattern).
      size_t from = out.size() - distance;
      for (size_t k = 0; k < length; ++k) out.push_back(out[from + k]);
    }
  }
}

const Huffman& fixedLiteral() {
  static const Huffman h = [] {
    uint8_t len[288];
    for (int s = 0; s < 144; ++s) len[s] = 8;
    for (int s = 144; s < 256; ++s) len[s] = 9;
    for (int s = 256; s < 280; ++s) len[s] = 7;
    for (int s = 280; s < 288; ++s) len[s] = 8;
    Huffman t;
    t.build(len, 288);
    return t;
  }();
  return h;
}

const Huffman& fixedDistance() {
  static const Huffman h = [] {
    uint8_t len[30];
    std::memset(len, 5, sizeof(len));
    Huffman t;
    t.build(len, 30);
    return t;
  }();
  return h;
}

// Dynamic block header: code lengths are themselves Huffman coded (RFC 1951 section 3.2.7).
bool readDynamicCodes(BitReader& in, Huffman& lit, Huffman& dist, std::string& error) {
  const int nlen = static_cast<int>(in.bits(5)) + 257, ndist = static_cast<int>(in.bits(5)) + 1;
  const int ncode = static_cast<int>(in.bits(4)) + 4;
  if (nlen > 286 || ndist > 30) return error = "bad code counts", false;
  static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  uint8_t lengths[320] = {};
  for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<uint8_t>(in.bits(3));
  Huffman lencode;
  if (!lencode.build(lengths, 19)) return error = "bad code length code", false;

  int index = 0;
  while (index < nlen + ndist) {
    const int sym = lencode.decode(in);
    if (sym < 0 || in.pastEnd()) return error = "bad code lengths", false;
    if (sym < 16) {
      lengths[index++] = static_cast<uint8_t>(sym);
      continue;
    }
    uint8_t value = 0;
    int repeat;
    if (sym == 16) {  // repeat the previous length 3..6 times
      if (index == 0) return error = "repeat with no previous length", false;
      value = lengths[index - 1];
      repeat = 3 + static_cast<int>(in.bits(2));
    } else if (sym == 17) {
      repeat = 3 + static_cast<int>(in.bits(3));  // 3..10 zeros
    } else {
      repeat = 11 + static_cast<int>(in.bits(7));  // 11..138 zeros
    }
    if (index + repeat > nlen + ndist) return error = "too many code lengths", false;
    while (repeat--) lengths[index++] = value;
  }
  if (lengths[256] == 0) return error = "no end-of-block code", false;
  if (!lit.build(lengths, nlen) || !dist.build(lengths + nlen, ndist)) return error = "bad literal/distance code", false;
  return true;
}

}  // namespace

bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error, size_t* consumed) {
  BitReader in{data, size};
  Huffman lit, dist;
  bool last = false;
  while (!last) {
    last = in.bits(1) != 0;
    const uint32_t type = in.bits(2);
    if (in.pastEnd()) return error = "truncated stream", false;
    if (type == 0) {  // stored: byte aligned LEN, ~LEN, then raw bytes
      in.alignToByte();
      const uint32_t len = in.bits(16), nlen = in.bits(16);
      if (len != (~nlen & 0xFFFFu)) return error = "stored block length mismatch", false;
      for (uint32_t k = 0; k < len; ++k) out.push_back(static_cast<uint8_t>(in.bits(8)));
      if (in.pastEnd()) return error = "truncated stored block", false;
    } else if (type == 1) {
      if (!decodeBlock(in, fixedLiteral(), fixedDistance(), out, error)) return false;
    } else if (type == 2) {
      if (!readDynamicCodes(in, lit, dist, error) || !decodeBlock(in, lit, dist, out, error)) return false;
    } else {
      return error = "invalid block type", false;
    }
  }
  in.alignToByte();
  if (consumed) *consumed = in.bytesConsumed();
  return true;
}

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

bool gunzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error) {
  if (size < 18 || !isGzip(data, size)) return error = "not a gzip stream", false;
  if (data[2] != 8) return error = "unknown gzip compression method", false;
  const uint8_t flags = data[3];
  size_t pos = 10;
  auto skipZeroTerminated = [&] {
    while (pos < size && data[pos] != 0) ++pos;
    ++pos;
  };
  if (flags & 4) {  // FEXTRA
    if (pos + 2 > size) return error = "truncated gzip header", false;
    pos += 2 + (data[pos] | data[pos + 1] << 8);
  }
  if (flags & 8) skipZeroTerminated();   // FNAME
  if (flags & 16) skipZeroTerminated();  // FCOMMENT
  if (flags & 2) pos += 2;               // FHCRC
  if (pos >= size) return error = "truncated gzip header", false;

  out.clear();
  // The trailer stores the uncompressed size (mod 2^32): reserve it up front.
  const uint32_t expected = uint32_t(data[size - 4]) | uint32_t(data[size - 3]) << 8 | uint32_t(data[size - 2]) << 16 | uint32_t(data[size - 1]) << 24;
  out.reserve(std::min<size_t>(expected, size_t(1) << 30));
  size_t used = 0;
  if (!inflateRaw(data + pos, size - pos, out, error, &used)) return error = "gzip: " + error, false;
  pos += used;
  if (pos + 8 > size) return error = "gzip: missing trailer (truncated file)", false;
  auto le32 = [&](size_t p) { return uint32_t(data[p]) | uint32_t(data[p + 1]) << 8 | uint32_t(data[p + 2]) << 16 | uint32_t(data[p + 3]) << 24; };
  if (le32(pos) != crc32(out.data(), out.size())) return error = "gzip: CRC-32 mismatch (corrupted file)", false;
  if (le32(pos + 4) != static_cast<uint32_t>(out.size())) return error = "gzip: length mismatch", false;
  return true;
}

}  // namespace gsv
