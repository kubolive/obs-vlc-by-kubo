# obs-vlc-by-kubo

Bản tùy biến nâng cao cho plugin `vlc-video` (OBS VLC Source) của OBS Studio.

---

> [!WARNING]
> ### CẢNH BÁO QUAN TRỌNG / IMPORTANT WARNING
> - **Tiếng Việt:** Việc cài đặt plugin này sẽ **ghi đè trực tiếp lên plugin `obs-vlc` (`vlc-video`) mặc định** của OBS Studio. Vui lòng sao lưu file `vlc-video` gốc trước khi thay thế.
> - **English:** Installing this plugin will **overwrite the default `obs-vlc` (`vlc-video`) plugin** of OBS Studio. Please back up your original `vlc-video` plugin file before replacing.

---

## Những thay đổi đã thực hiện / What was done

1. **Chuyển toàn bộ chuỗi sang file ngôn ngữ (`.ini`)**:
   - Tất cả nhãn giao diện, tùy chọn danh sách thả xuống và thông báo xác nhận hộp thoại được đưa vào hệ thống đa ngôn ngữ chuẩn của OBS Studio (`data/locale/en-US.ini` và `data/locale/vi-VN.ini`).
   - Không còn hardcode bất kỳ chuỗi text nào trong mã nguồn C.

2. **Ghép kênh âm thanh đa rãnh MXF (Audio Track Pairing)**:
   - Tự động nhận diện cấu hình âm thanh của file MXF phát sóng (Broadcast Playout).
   - Tự động ghép cặp các rãnh mono rời rạc thành cặp stereo: Kênh 1-2 (L+R), Kênh 1-3, Kênh 3-4,...

3. **Tùy biến hiển thị phụ đề (Custom Subtitles)**:
   - Cho phép tùy chỉnh phông chữ, màu chữ.
   - Bật/tắt và chọn độ dày viền chữ (Outline), màu viền.
   - Bật/tắt bóng đổ (Shadow), chỉnh khoảng cách và độ mờ bóng.
   - Bật/tắt khung bao quanh chữ (Background Box), chọn màu và chỉnh độ trong suốt khung.
   - Chỉnh lề dưới (Bottom margin) và tùy chọn ghi đè kiểu dáng ASS/SSA nhúng.

4. **Chuyển đổi rãnh âm thanh & phụ đề không làm gián đoạn phát sóng**:
   - Thay đổi Audio Track hoặc Subtitle Track ngay trong hộp thoại Properties mà không restart phát sóng.
   - Giữ nguyên vị trí thời gian (PTS), trạng thái danh sách phát và phiên media hiện tại.

5. **Thao tác Playlist mượt mà (Playlist Operations)**:
   - Thêm tệp mới vào danh sách mà không làm ngắt quãng video đang phát.
   - Xóa các tệp khác trong danh sách phát mà không gây gián đoạn playback.
   - Hiển thị hộp thoại xác nhận cảnh báo khi xóa tệp đang được phát sóng trực tiếp.
   - Làm sạch danh sách phát (Clear Playlist) dừng và giải phóng tài nguyên an toàn.

---

## Cài đặt / Installation

### Windows
1. Đóng hoàn toàn OBS Studio.
2. Tải bản build mới nhất từ tab **Actions** hoặc **Releases** (`obs-vlc-by-kubo-windows-x64.zip`).
3. Sao lưu file `vlc-video.dll` trong `C:\Program Files\obs-studio\obs-plugins\64bit\`.
4. Chép đè `vlc-video.dll` mới vào `C:\Program Files\obs-studio\obs-plugins\64bit\`.
5. Chép các file trong `data/obs-plugins/vlc-video/locale/` vào `C:\Program Files\obs-studio\data\obs-plugins\vlc-video\locale\`.
6. Khởi động lại OBS Studio.

### Linux
1. Đóng OBS Studio.
2. Sao lưu file `vlc-video.so` trong `/usr/lib/x86_64-linux-gnu/obs-plugins/` hoặc `~/.config/obs-studio/plugins/vlc-video/bin/64bit/`.
3. Chép đè `vlc-video.so` và thư mục `locale/` vào đúng vị trí tương ứng.

### macOS
1. Đóng OBS Studio.
2. Chép đè `vlc-video.so` vào `OBS.app/Contents/PlugIns/` hoặc thư mục Application Support của OBS.

---

## Build từ mã nguồn / Building from Source

Dự án hỗ trợ build tự động qua **GitHub Actions** cho 3 nền tảng: **Windows (x64), Linux (x86_64), macOS (Universal: x86_64 & arm64)**.

### Build thủ công bằng CMake

```bash
# Cấu hình
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Biên dịch
cmake --build build --config Release
```

---

## License

GNU General Public License v2.0 (GPL-2.0) - Cùng giấy phép với OBS Studio.
