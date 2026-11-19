# Lộ trình 7 ngày

Mỗi ngày kết thúc bằng: **chạy được, chụp ảnh `--screenshot`, ghi số đo vào BENCHMARKS.md, commit**.
Lịch sử commit đều đặn theo ngày là bằng chứng nhà tuyển dụng nhìn thấy được.

---

## ✅ Ngày 1: Nền móng (đã xong)

- CMake + FetchContent (GLFW, GLM, ImGui), build ra một file `.exe` tĩnh
- Bộ nạp hàm OpenGL tự viết ([gl_loader.h](../src/gl/gl_loader.h))
- Đọc/ghi PLY chuẩn 3DGS ([ply_io.cpp](../src/scene/ply_io.cpp)), cảnh mẫu tự sinh
- Camera orbit/pan/fly, đo GPU time, hot-reload shader, chế độ chụp ảnh không cần UI
- Render mỗi Gaussian thành một điểm

**Commit gợi ý:** `feat: project setup, PLY loader, camera and point renderer`

---

## Ngày 2: Từ Gaussian 3D thành elip 2D (phần toán cốt lõi)

**Mục tiêu:** mỗi Gaussian hiện thành một hình elip đúng hình dạng và hướng (chưa cần trộn màu đúng).

**Việc cần làm**
1. Tạo `render/splat_renderer.{h,cpp}` và `shaders/splat.vert`, `shaders/splat.frag`.
2. Đưa dữ liệu lên GPU bằng **SSBO** (`GL_SHADER_STORAGE_BUFFER`), mỗi Gaussian một struct `{pos, scale, rot, opacity, color}`.
3. Vẽ bằng instancing: `glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, N)`; mỗi instance là một quad, `gl_InstanceID` chọn Gaussian.
4. Trong vertex shader:
   - `R` = ma trận quay từ quaternion (nhớ chuẩn hóa), `S = diag(exp(logScale))`
   - Hiệp phương sai 3D: **Σ = R·S·Sᵀ·Rᵀ**
   - Đổi sang hệ camera: `t = view * pos`, `W` = phần 3×3 của view
   - Jacobian của phép chiếu phối cảnh tại `t` (f = tiêu cự tính bằng pixel, `camera.focalPixels()`):
     ```
     J = | f/tz   0     -f·tx/tz² |
         | 0      f/tz  -f·ty/tz² |
     ```
   - Hiệp phương sai 2D: **Σ' = J·W·Σ·Wᵀ·Jᵀ**, cộng `0.3` vào đường chéo (chống răng cưa, giống bản gốc)
   - Tính 2 trị riêng λ₁, λ₂ của Σ' (ma trận 2×2 đối xứng) → bán kính `3·sqrt(λ)` → kích thước quad
   - Truyền ma trận nghịch đảo `conic = Σ'⁻¹` xuống fragment shader
5. Trong fragment shader: `d` = khoảng cách pixel tới tâm, `power = -0.5·dᵀ·conic·d`, `alpha = opacity·exp(power)`, bỏ pixel nếu `alpha < 1/255`.

**Bẫy hay gặp**
- Quaternion trong file là `(w, x, y, z)` = `rot_0..rot_3`
- `tz` âm trong OpenGL (camera nhìn về −Z): dùng `-t.z` làm độ sâu, loại Gaussian phía sau camera
- Gaussian ở rìa màn hình làm Jacobian "nổ": kẹp `tx/tz`, `ty/tz` lại (xem `jacobian-pinhole.ts` của splat-transform)

**Tham khảo:** `splat-transform/src/lib/gpu/shaders/chunks/` → `quat-rotation.ts`, `covariance-3d.ts`, `jacobian-pinhole.ts`

**Kiểm chứng:** cảnh mẫu → sàn phải phủ kín bằng các elip dẹt, quả cầu phải tròn khi xoay.

**Commit:** `feat: EWA projection of 3D Gaussians to screen-space ellipses`

---

## Ngày 3: Sắp xếp + trộn màu → ảnh đúng (mốc quan trọng nhất)

**Mục tiêu:** ảnh render trông giống bản gốc. Đây là **mốc sản phẩm tối thiểu**: nếu tuần bị trễ, giữ chắc mốc này.

**Việc cần làm**
1. Mỗi khung hình: tính độ sâu `depth_i = dot(viewRow2, pos_i)` cho mọi Gaussian, sắp xếp chỉ số theo độ sâu (CPU, `std::sort`), upload mảng chỉ số lên SSBO.
2. Vẽ theo thứ tự **từ xa đến gần**, blending `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` với màu đã nhân alpha (premultiplied).
3. Tắt ghi depth (`glDepthMask(GL_FALSE)`) khi vẽ splat.
4. Chỉ sắp xếp lại khi camera thay đổi; đo thời gian sắp xếp riêng (CPU timer).

**Tối ưu nhỏ đáng làm:** radix sort trên CPU với key độ sâu 16/32-bit, nhanh hơn `std::sort` nhiều lần. Ghi cả hai con số vào BENCHMARKS.

**Kiểm chứng:** tải một cảnh thật (mô hình mẫu của INRIA, ví dụ `bicycle`, `garden`) với `--flip-y`.

**Commit:** `feat: depth sorting and alpha blending` · `perf: CPU radix sort (Xx faster than std::sort)`

---

## Ngày 4: Màu theo góc nhìn + sắp xếp trên GPU

1. **Spherical harmonics bậc 1–3:** đưa `f_rest` lên SSBO, tính trong vertex shader với `dir = normalize(pos - cameraPos)`.
   Hằng số: `C1 = 0.4886025`, `C2 = [1.0925484, -1.0925484, 0.3153916, -1.0925484, 0.5462742]`, `C3 = [...]` (xem `sh-band-*.ts` hoặc bài báo gốc).
   Thêm thanh trượt "SH degree 0–3" để thấy khác biệt (bóng loáng, phản chiếu).
2. **Radix sort bằng compute shader:** đếm histogram → prefix sum → scatter, 4 lượt × 8 bit. Toàn bộ dữ liệu không rời GPU.
3. So sánh: thời gian sắp xếp CPU vs GPU, FPS tổng.

**Tham khảo:** `splat-transform/src/lib/gpu/shaders/prefix-sum.ts`, `depth-keys.ts`; bài "Fast 4-way parallel radix sorting on GPUs".

**Commit:** `feat: view-dependent color with SH up to degree 3` · `perf: GPU radix sort in compute shaders`

---

## Ngày 5: Dữ liệu của bạn + công cụ debug

1. **Quét cảnh bằng điện thoại Android** (Scaniverse / Polycam / KIRI Engine, xuất `.ply` dạng splat).
   Nếu app xuất `.spz`/`.sog`: `npx @playcanvas/splat-transform scan.spz scan.ply`
2. **Frustum culling** trong compute shader: bỏ Gaussian ngoài tầm nhìn trước khi sắp xếp. Hiện số Gaussian đã loại trên UI.
3. **Chế độ debug:** viền elip, bản đồ độ sâu, heatmap mật độ chồng lớp (overdraw).
4. Tải file ở luồng riêng (`std::thread`) để UI không đứng khi mở file lớn.

**Commit:** `feat: frustum culling and debug views` · `feat: async scene loading`

---

## Ngày 6: Đo đạc và kiểm chứng

1. Tạo cảnh ở nhiều kích thước: `splat-transform scan.ply -d 25% scan_25.ply` (25/50/75/100%).
2. Điền bảng BENCHMARKS: số Gaussian vs GPU time vs thời gian sắp xếp; vẽ biểu đồ đưa vào README.
3. **So với renderer tham chiếu:**
   ```
   npx @playcanvas/splat-transform scene.ply ref.webp --camera-pos 2,1,-2 --camera-target 0,0,0 --resolution 1280x720
   ```
   Thêm tùy chọn `--camera-pos/--camera-target` cho viewer, chụp cùng góc, tính **PSNR** giữa hai ảnh (một script nhỏ hoặc thêm vào app).
4. Sửa lỗi, dọn code, bật `-Werror` nếu được.

**Commit:** `test: PSNR comparison against reference renderer` · `docs: benchmark results`

---

## Ngày 7: Trình bày

1. README: GIF/video ở đầu (ScreenToGif hoặc OBS), bảng hiệu năng, mục "How it works" có công thức.
2. Viết `docs/MATH.md` hoặc một bài blog: từ Σ 3D → Σ' 2D → alpha blending, kèm hình minh họa.
3. Mục "Limitations & future work": port Android (OpenGL ES 3.1 / Vulkan), LOD, nén dữ liệu.
4. Đưa lên GitHub, ghim (pin) repo trên profile, thêm topics: `gaussian-splatting`, `opengl`, `cpp`, `computer-graphics`.

**Commit:** `docs: README, demo video and math write-up`
