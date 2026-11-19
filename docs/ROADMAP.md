# Lộ trình 7 ngày (đã hoàn thành)

Tài liệu lý thuyết chi tiết cho từng ngày: `F:\GraphicsLearning\dayNN\DayNN-LyThuyet.pdf`.

| Ngày | Commit | Nội dung | Kết quả đo |
|---|---|---|---|
| 1 | `948a330` | CMake + FetchContent, GL loader (X-macro), PLY I/O, camera orbit, point renderer, GPU timer | 300 K điểm: 0,13 ms |
| 2 | `fcdf18e` | Tái cấu trúc (core / app / tests), Σ = RSSᵀRᵀ, EWA projection, compute preprocess, indirect draw | GPU khớp CPU 1,2·10⁻⁴ px |
| 3 | `b47d232` | Khóa độ sâu IEEE-754, std::sort và radix sort CPU, renderer tham chiếu CPU, PSNR | 54,4 dB; radix nhanh 5,3× |
| 4 | `1705071` | Spherical harmonics bậc 3, GPU radix sort (histogram, Blelloch scan, scatter), khóa 16 bit | 1 M khóa: 2,7 ms |
| 5 | `4c7ed64` | `.splat`, loại NaN, tải bất đồng bộ, culling, debug views | 38 test |
| 6 | `2509f12` | `--bench`, SH fp16, parallelFor, so với splat-transform | 1 M splat 1080p: 119 FPS; 40,6 dB |
| 7 | (commit cuối) | README, MATH.md, GIF, turntable, CI, `.gitattributes` | 42 test |

## Hướng phát triển tiếp

1. **Rasterizer chia ô bằng compute shader** với dừng sớm (front-to-back, T < 10⁻⁴): giảm chi phí vẽ, hiện là 58% khung hình.
2. **LOD / decimation** cho cảnh rất lớn (> 5 triệu splat).
3. **Port Android**: OpenGL ES 3.1 có compute shader và SSBO, nên hầu hết shader chuyển được gần nguyên vẹn
   (đổi `#version 310 es`, thêm `precision`). Bộ nạp hàm thay bằng EGL; GLFW thay bằng `android_native_app_glue`.
4. **Huấn luyện**: tự viết bước tối ưu Gaussian từ ảnh (cần đạo hàm ngược của toàn bộ pipeline).
