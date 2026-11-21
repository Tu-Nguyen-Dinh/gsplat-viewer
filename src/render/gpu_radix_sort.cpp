#include "render/gpu_radix_sort.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace gsv {

namespace {
constexpr uint32_t kBlock = 1024;     // RADIX_BLOCK in radix_common.glsl
constexpr uint32_t kBuckets = 16;     // RADIX_BUCKETS
constexpr uint32_t kRadixBits = 4;
constexpr uint32_t kScanBlock = 512;  // elements per scan_local work group
constexpr GLuint kBindCount = 7;

uint32_t divUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
}  // namespace

bool GpuRadixSort::init(std::string& log) {
  return histogram_.loadCompute("radix_histogram.comp", log) && scatter_.loadCompute("radix_scatter.comp", log) &&
         scanLocal_.loadCompute("scan_local.comp", log) && scanAdd_.loadCompute("scan_add.comp", log);
}

bool GpuRadixSort::reloadShaders(std::string& log) {
  std::string a, b, c, d;
  // Reload all four (no short-circuit) so every compile error is reported at once.
  const bool ok1 = histogram_.reload(a), ok2 = scatter_.reload(b), ok3 = scanLocal_.reload(c), ok4 = scanAdd_.reload(d);
  const bool ok = ok1 && ok2 && ok3 && ok4;
  log = a + b + c + d;
  return ok;
}

void GpuRadixSort::ensureCapacity(uint32_t capacity) {
  if (capacity <= capacity_) return;
  capacity_ = capacity;
  const uint32_t numBlocks = divUp(capacity, kBlock);
  const uint32_t histLength = numBlocks * kBuckets;
  keysTmp_.allocate(size_t(capacity) * sizeof(uint32_t));
  valuesTmp_.allocate(size_t(capacity) * sizeof(uint32_t));
  blockHist_.allocate(size_t(histLength) * sizeof(uint32_t));
  blockSums_.allocate(size_t(divUp(histLength, kScanBlock)) * sizeof(uint32_t));
  blockSumsTotal_.allocate(sizeof(uint32_t));
}

// Exclusive scan of blockHist_[0..length) with a two-level Blelloch scan.
void GpuRadixSort::scan(uint32_t length) {
  const uint32_t groups = divUp(length, kScanBlock);
  assert(groups <= kScanBlock && "too many elements for a two-level scan");

  scanLocal_.use();
  scanLocal_.set("uCount", length);
  blockHist_.bindBase(GL_SHADER_STORAGE_BUFFER, 0);
  blockSums_.bindBase(GL_SHADER_STORAGE_BUFFER, 1);
  glDispatchCompute(groups, 1, 1);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  if (groups == 1) return;

  // Scan the block totals (fits in one work group), then add them back.
  scanLocal_.set("uCount", groups);
  blockSums_.bindBase(GL_SHADER_STORAGE_BUFFER, 0);
  blockSumsTotal_.bindBase(GL_SHADER_STORAGE_BUFFER, 1);
  glDispatchCompute(1, 1, 1);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

  scanAdd_.use();
  scanAdd_.set("uCount", length);
  blockHist_.bindBase(GL_SHADER_STORAGE_BUFFER, 0);
  blockSums_.bindBase(GL_SHADER_STORAGE_BUFFER, 1);
  glDispatchCompute(groups, 1, 1);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

void GpuRadixSort::sort(GlBuffer& keys, GlBuffer& values, const GlBuffer& countBuffer, uint32_t countIndex,
                        uint32_t capacity, int keyBits) {
  if (capacity == 0) return;
  assert(capacity <= kMaxElements);
  ensureCapacity(capacity);
  const uint32_t numBlocks = divUp(capacity, kBlock);
  const uint32_t passes = static_cast<uint32_t>(keyBits) / kRadixBits;  // 8 or 4: always even

  GlBuffer* srcK = &keys;
  GlBuffer* srcV = &values;
  GlBuffer* dstK = &keysTmp_;
  GlBuffer* dstV = &valuesTmp_;
  countBuffer.bindBase(GL_SHADER_STORAGE_BUFFER, kBindCount);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

  for (uint32_t pass = 0; pass < passes; ++pass) {
    const uint32_t shift = pass * kRadixBits;

    histogram_.use();
    histogram_.set("uCountIndex", countIndex);
    histogram_.set("uShift", shift);
    histogram_.set("uNumBlocks", numBlocks);
    srcK->bindBase(GL_SHADER_STORAGE_BUFFER, 0);
    blockHist_.bindBase(GL_SHADER_STORAGE_BUFFER, 4);
    glDispatchCompute(numBlocks, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    scan(numBlocks * kBuckets);
    countBuffer.bindBase(GL_SHADER_STORAGE_BUFFER, kBindCount);

    scatter_.use();
    scatter_.set("uCountIndex", countIndex);
    scatter_.set("uShift", shift);
    scatter_.set("uNumBlocks", numBlocks);
    srcK->bindBase(GL_SHADER_STORAGE_BUFFER, 0);
    srcV->bindBase(GL_SHADER_STORAGE_BUFFER, 1);
    dstK->bindBase(GL_SHADER_STORAGE_BUFFER, 2);
    dstV->bindBase(GL_SHADER_STORAGE_BUFFER, 3);
    blockHist_.bindBase(GL_SHADER_STORAGE_BUFFER, 4);
    glDispatchCompute(numBlocks, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    std::swap(srcK, dstK);
    std::swap(srcV, dstV);
  }
  // An even number of passes leaves the result in the caller's buffers.
}

}  // namespace gsv
