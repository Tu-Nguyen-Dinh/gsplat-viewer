# Trình bày dự án trong CV và phỏng vấn

> Nguyên tắc: **chỉ ghi những gì bạn đã tự làm và đo được**. Các ô `[ ]` dưới đây điền bằng số thật sau khi hoàn thành.

## 1. Mục "Projects" trong CV (tiếng Anh)

**gsplat-viewer: Real-time 3D Gaussian Splatting Renderer** · C++20, OpenGL 4.3, GLSL, CMake · [github.com/<you>/gsplat-viewer]

- Built a real-time renderer for 3D Gaussian Splatting scenes from scratch in C++20/OpenGL, rendering [1.2M] Gaussians at [X] FPS (1080p, RTX 3060).
- Implemented EWA splatting on the GPU: 3D covariance from scale/rotation, projection to screen-space ellipses via the perspective Jacobian, and view-dependent color with spherical harmonics up to degree 3.
- Wrote a GPU radix sort in compute shaders for per-frame depth ordering, cutting sort time from [X] ms (CPU `std::sort`) to [Y] ms.
- Validated output against PlayCanvas' reference renderer ([Z] dB PSNR) and documented benchmarks across scene sizes.
- Reconstructed and rendered my own phone-captured scenes; added frustum culling, debug views and async loading.

Chọn 3–4 gạch đầu dòng mạnh nhất. Mỗi dòng: **động từ hành động + kỹ thuật + con số**.

## 2. Phiên bản tiếng Việt (CV nộp công ty trong nước)

**gsplat-viewer: Renderer 3D Gaussian Splatting thời gian thực** · C++20, OpenGL, GLSL
- Tự xây dựng renderer cho cảnh 3D Gaussian Splatting, hiển thị [1,2 triệu] Gaussian ở [X] FPS.
- Hiện thực phép chiếu EWA (hiệp phương sai 3D → elip 2D) và màu phụ thuộc góc nhìn bằng spherical harmonics trên GPU.
- Viết radix sort bằng compute shader, giảm thời gian sắp xếp từ [X] ms xuống [Y] ms.
- Kiểm chứng kết quả với renderer tham chiếu của PlayCanvas (PSNR [Z] dB).

## 3. Kỹ năng thể hiện qua dự án (mục "Skills")

C++20 · OpenGL 4.3 / GLSL · Compute shaders · GPU profiling · Linear algebra for graphics · CMake · Git

## 4. Câu hỏi phỏng vấn nên chuẩn bị

| Câu hỏi | Ý chính để trả lời |
|---|---|
| Vì sao phải sắp xếp Gaussian? | Alpha blending không giao hoán; cần đúng thứ tự xa→gần để kết quả đúng |
| Vì sao chiếu Gaussian 3D thành elip 2D là xấp xỉ? | Phép chiếu phối cảnh không tuyến tính; dùng khai triển bậc 1 (Jacobian) quanh tâm |
| Vì sao lưu `log(scale)` và `logit(opacity)`? | Để quá trình huấn luyện tối ưu không ràng buộc; khi render áp `exp`, `sigmoid` |
| Nút thắt hiệu năng là gì? | Sắp xếp mỗi khung hình và overdraw (fill-rate); dẫn chứng bằng số đo của bạn |
| Bạn đo hiệu năng thế nào? | `GL_TIME_ELAPSED` query, vòng nhiều query để không chặn CPU |
| Vì sao tự viết GL loader? | Hiểu cơ chế nạp hàm driver; ít phụ thuộc; build không cần Python |
| Nếu chạy trên điện thoại thì sao? | OpenGL ES 3.1 có compute; giảm số Gaussian (LOD), nén dữ liệu, giảm độ phân giải |
| Bạn tham khảo những gì? | Bài báo 3DGS/EWA, đọc code splat-transform; tự viết lại, dùng nó làm chuẩn so sánh |

## 5. GitHub: những gì nhà tuyển dụng nhìn vào

1. **README trong 30 giây đầu:** GIF chạy mượt → một câu mô tả → bảng hiệu năng.
2. **Lịch sử commit:** commit nhỏ, đều theo ngày, message rõ (`feat:`, `perf:`, `fix:`, `docs:`). Không dồn một commit khổng lồ.
3. **Code sạch:** chia module rõ, không có code chết, build được ngay theo hướng dẫn.
4. **Trung thực:** ghi rõ phần tham khảo. Đừng sao chép code shader của người khác.

## 6. Nhật ký học tập (điền mỗi ngày, rất có ích khi phỏng vấn)

| Ngày | Đã làm | Khó khăn | Cách giải quyết | Số đo |
|---|---|---|---|---|
| 1 | Setup, PLY, camera, point renderer | Không có Python để sinh glad; `git checkout` của stb bị treo | Tự viết GL loader bằng X-macro; vendor `stb_image_write.h` | 300k điểm, 0,13 ms GPU |
| 2 |  |  |  |  |
| 3 |  |  |  |  |
| 4 |  |  |  |  |
| 5 |  |  |  |  |
| 6 |  |  |  |  |
| 7 |  |  |  |  |
