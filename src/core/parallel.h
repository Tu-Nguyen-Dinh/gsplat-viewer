#pragma once
// Minimal data-parallel loop over [0, count) on all CPU cores.

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

namespace gsv {

// Calls fn(begin, end) on contiguous chunks, one chunk per hardware thread.
// fn must only write to its own range (no shared mutable state => no locks needed).
template <typename Fn>
void parallelFor(size_t count, Fn&& fn, size_t minChunk = 16384) {
  const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
  const size_t chunks = std::clamp<size_t>(count / std::max<size_t>(minChunk, 1), 1, hw);
  if (chunks == 1) {
    fn(size_t(0), count);
    return;
  }
  std::vector<std::thread> threads;
  threads.reserve(chunks - 1);
  const size_t step = (count + chunks - 1) / chunks;
  for (size_t c = 1; c < chunks; ++c) {
    const size_t b = c * step, e = std::min(count, b + step);
    if (b < e) threads.emplace_back([&fn, b, e] { fn(b, e); });
  }
  fn(size_t(0), std::min(count, step));  // the calling thread does the first chunk
  for (auto& t : threads) t.join();
}

}  // namespace gsv
