# Trình bày dự án trong CV và phỏng vấn

> Nguyên tắc: **chỉ ghi những gì bạn hiểu, giải thích được và đo được.** Mọi con số dưới đây là số đo
> thật trên RTX 3060 / i5-12400F (xem `docs/BENCHMARKS.md`).

## 0. Trung thực về vai trò của AI (đọc trước)

Ngày 1 dựng nền móng cùng nhau; từ Ngày 2 đến Ngày 7, phần lớn code được viết bởi trợ lý AI (Claude) theo
yêu cầu của bạn, kèm 6 tài liệu lý thuyết trong `F:\GraphicsLearning`. Nhà tuyển dụng đồ họa sẽ hỏi sâu
(vì sao Jacobian có dạng đó, vì sao radix sort phải ổn định, vì sao fp16 nhanh 4 lần...). Vì vậy:

1. **Học xong 7 tài liệu và làm bài tập** trước khi đưa dự án vào CV.
2. **Tự viết lại ít nhất một phần lõi** mà không nhìn code (gợi ý: `radix_scatter.comp` hoặc `projectGaussian`),
   chạy test để kiểm tra. Đây là cách nhanh nhất để biến code "được viết hộ" thành kiến thức của bạn.
3. **Nói rõ khi được hỏi**: "Tôi xây dựng dự án với AI làm pair-programmer; tôi chịu trách nhiệm thiết kế,
   kiểm thử, đo đạc và hiểu từng phần". Đây là cách làm việc phổ biến hiện nay; che giấu mới là rủi ro.
4. Ghi trong README phần "Acknowledgements" nếu bạn muốn minh bạch tuyệt đối.

## 1. Mục "Projects" (tiếng Anh)

**gsplat-viewer — Real-time 3D Gaussian Splatting renderer** · C++20, OpenGL 4.3, GLSL compute, CMake · github.com/<you>/gsplat-viewer

- Built a real-time 3D Gaussian Splatting renderer in C++20/OpenGL 4.3 (AI-assisted): **1 M Gaussians at 1080p in 8.4 ms (119 FPS)** on an RTX 3060.
- Implemented EWA projection (3D covariance → screen-space ellipses via the perspective Jacobian) and degree-3 spherical harmonics in compute shaders; GPU output matches a CPU reference renderer at **54 dB PSNR** and PlayCanvas splat-transform at **40.6 dB**.
- Wrote a stable GPU radix sort (histogram, Blelloch scan, packed local ranking) with GPU-resident counts and indirect drawing: **2.5 ms for 1 M keys vs 60 ms with `std::sort`**.
- Profiled with GPU timer queries and fragment counters; float16 SH storage made preprocessing **4.1× faster**, oriented quads cut rasterized fragments **2.2×**.
- Set up 42 CPU/GPU tests (numerical Jacobian, exhaustive float16, image PSNR), a benchmark mode with CSV output, async loading and `.ply`/`.splat` support.

Chọn 3–4 dòng mạnh nhất cho CV một trang. Mỗi dòng: **động từ + kỹ thuật + con số**.

## 2. Phiên bản tiếng Việt

**gsplat-viewer — Renderer 3D Gaussian Splatting thời gian thực** · C++20, OpenGL, GLSL
- Xây dựng renderer 3DGS (có hỗ trợ của AI): 1 triệu Gaussian ở 1080p, 119 FPS trên RTX 3060.
- Phép chiếu EWA và spherical harmonics bậc 3 trên GPU; khớp renderer tham chiếu 54 dB PSNR, khớp splat-transform 40,6 dB.
- Radix sort ổn định bằng compute shader: 2,5 ms cho 1 triệu khóa (std::sort: 60 ms).
- Đo hiệu năng bằng GPU timer query; lưu SH dạng fp16 tăng tốc 4,1 lần; 42 test CPU/GPU.

## 3. Kỹ năng (mục "Skills")

C++20 · OpenGL 4.3 / GLSL · Compute shaders · GPU parallel algorithms (scan, radix sort) · GPU profiling ·
Linear algebra for graphics · Testing numerical code · CMake · Git

## 4. Câu hỏi phỏng vấn và ý trả lời

| Câu hỏi | Ý chính | Tài liệu |
|---|---|---|
| Đi qua pipeline render một khung hình | Upload 1 lần → preprocess (chiếu, SH, cull, append) → GPU sort → indirect instanced draw, blend back-to-front | Ngày 2, 4 |
| Vì sao Σ = R S Sᵀ Rᵀ? | Biến đổi Gaussian chuẩn bởi M = RS; luôn hợp lệ khi tối ưu | Ngày 2 ch.4 |
| Jacobian ở đâu ra? | Phép chiếu phối cảnh không affine; khai triển Taylor bậc 1 quanh tâm | Ngày 2 ch.5 |
| Vì sao phải sắp xếp? Depth test không được à? | "Over" không giao hoán; splat bán trong suốt | Ngày 2 ch.8, Ngày 3 ch.2 |
| Radix sort GPU hoạt động thế nào? Vì sao phải ổn định? | Histogram digit-major → scan → scatter; LSD cần ổn định để giữ thứ tự chữ số thấp | Ngày 3 ch.5, Ngày 4 ch.6 |
| Bottleneck của renderer? | Vẽ ~58%, fill-rate, ~57 lớp/pixel; hướng tiếp: tile-based + early termination | Ngày 6 ch.3, 7 |
| Vì sao fp16 nhanh 4 lần mà dữ liệu chỉ giảm 2 lần? | Memory coalescing, cache line, L2 | Ngày 6 ch.4 |
| Quad xoay nhanh bao nhiêu? | Fragment ít hơn 2,2×, nhưng chỉ nhanh 1,18× vì fragment bị discard rẻ | Ngày 6 ch.5 |
| Làm sao biết kết quả đúng? | CPU reference (PSNR 54 dB), test "phải sai" (23 dB), oracle độc lập splat-transform (40,6 dB) | Ngày 3, 6 |
| Đưa lên điện thoại thì sao? | OpenGL ES 3.1 có compute + SSBO; giảm độ phân giải, LOD, khóa 16 bit, SH fp16 | Ngày 7 |

## 5. GitHub

1. README: GIF ở đầu, một câu mô tả, bảng hiệu năng (đã có).
2. Lịch sử commit theo ngày, message rõ ràng (đã có: 7 commit `feat:` / `perf:` / `docs:`).
3. Sửa email git (`gamil.com` → `gmail.com`) trước khi push để commit gắn với tài khoản.
4. Pin repo trên profile; thêm topics: `gaussian-splatting`, `opengl`, `cpp`, `computer-graphics`, `gpu`.
