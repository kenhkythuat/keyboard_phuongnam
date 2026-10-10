# Hướng dẫn cập nhật firmware OTA

Tài liệu này mô tả đúng quy trình OTA đang được firmware `KEYBOARD_PHUONG_NAM` sử dụng. Thực hiện lần lượt để tránh sai version, sai nhánh GitHub, HTTP 404 hoặc firmware bị từ chối vì descriptor không khớp.

## 1. Cấu hình OTA hiện tại

| Thành phần | Giá trị hiện tại |
|---|---|
| Repository | `kenhkythuat/keyboard_phuongnam` |
| Nhánh phát hành | `feature/control_kepad` |
| Manifest | `OTA/version.json` |
| Firmware binary | `OTA/file.bin` |
| Manifest URL | `https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam/feature/control_kepad/OTA/version.json` |
| Firmware URL | `https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam/feature/control_kepad/OTA/file.bin` |
| Project name trong image | `KEYBOARD_PHUONG_NAM` |
| Version nguồn | `PROJECT_VER` trong `CMakeLists.txt` |
| OTA slot | `ota_0` và `ota_1`, mỗi slot `0x300000` byte |
| Rollback bootloader | Đã bật |

Các URL được tạo từ `OTA_RELEASE_BRANCH` trong `main/device_config.h`. Repository phải public vì firmware hiện không gửi GitHub credential.

## 2. Nguyên tắc bắt buộc

1. Version mới phải lớn hơn version đang chạy trên thiết bị.
2. Chỉ sửa `PROJECT_VER` trong `CMakeLists.txt`; không sửa version nhúng trong binary bằng tay.
3. Phải build lại sau khi đổi `PROJECT_VER`.
4. Phải chạy `tools/prepare_ota.py` sau khi build.
5. `OTA/file.bin` và `OTA/version.json` phải được tạo từ cùng một lần build.
6. Phải push cả hai file lên đúng nhánh mà firmware đang cấu hình.
7. Không đổi tên `file.bin` hoặc `version.json`.
8. Không gửi command OTA trước khi Raw GitHub trả đúng manifest và binary mới.
9. Không dùng `erase_flash` trong quá trình OTA; NVS nằm ngoài hai OTA app slot và cần được giữ lại.

Firmware chỉ chấp nhận `firmware_url` trong manifest nếu URL khớp chính xác với `OTA_FIRMWARE_URL`. URL khác, kể cả cùng trỏ tới một file, vẫn bị từ chối.

## 3. Bước 1 - Chọn version mới

Kiểm tra version đang chạy từ log boot:

```text
OTA_MANAGER: OTA manager started, current_version=1.4
```

Hoặc xem field `version` trong heartbeat/telemetry.

Mở `CMakeLists.txt` và tăng version. Ví dụ:

```cmake
set(PROJECT_VER "1.5")
```

Version hợp lệ là chuỗi số có từ 2 đến 4 phần, ví dụ:

```text
1.5
1.5.0
1.5.0.1
```

Không dùng các dạng:

```text
v1.5
1.5-beta
release-1.5
```

## 4. Bước 2 - Build firmware mới

Mở ESP-IDF terminal tại thư mục project:

```powershell
cd C:\Espressif\frameworks\KEYBOARD_PHUONG_NAM
idf.py build
```

Build phải kết thúc thành công và tạo file:

```text
build/KEYBOARD_PHUONG_NAM.bin
```

Kiểm tra log cuối build phải cho biết binary nhỏ hơn OTA partition 3 MB. Không tiếp tục nếu có lỗi compile, link hoặc partition overflow.

Không dùng một file `.bin` từ lần build cũ sau khi vừa đổi `PROJECT_VER`.

## 5. Bước 3 - Đóng gói OTA

Chạy script với đúng nhánh phát hành:

```powershell
python tools/prepare_ota.py --branch feature/control_kepad
```

Script thực hiện các việc sau:

1. Đọc `PROJECT_VER` trong `CMakeLists.txt`.
2. Đọc version thật được nhúng trong `build/KEYBOARD_PHUONG_NAM.bin`.
3. Dừng nếu hai version không khớp.
4. Copy binary thành `OTA/file.bin`.
5. Tạo lại `OTA/version.json` với URL đúng nhánh.

Kết quả mong đợi:

```text
Prepared .../OTA/file.bin (... bytes), version 1.5, branch feature/control_kepad
Commit and push OTA/file.bin and OTA/version.json together.
```

Manifest được tạo có dạng:

```json
{
  "version": "1.5",
  "firmware_url": "https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam/feature/control_kepad/OTA/file.bin",
  "size": 1168064
}
```

Không chỉnh riêng `version` trong JSON sau bước này. Field `size` hiện dùng để tham khảo; firmware xác nhận dữ liệu tải hoàn chỉnh và kiểm tra ESP image khi cài đặt.

## 6. Bước 4 - Kiểm tra trước khi push

Chạy:

```powershell
git status --short
git diff -- CMakeLists.txt OTA/version.json
```

Phải thấy ít nhất:

```text
M CMakeLists.txt
M OTA/file.bin
M OTA/version.json
```

Xác nhận ba version giống nhau:

```text
CMakeLists.txt PROJECT_VER
OTA/version.json version
Version nhúng trong OTA/file.bin đã được prepare_ota.py xác nhận
```

## 7. Bước 5 - Commit và push GitHub

Commit source, binary và manifest của cùng release:

```powershell
git add CMakeLists.txt OTA/file.bin OTA/version.json
git commit -m "release: firmware 1.5"
git push origin feature/control_kepad
```

Nếu release có thay đổi source khác, thêm đúng các file source đó vào cùng commit.

Sau khi push, mở hai URL sau bằng trình duyệt:

```text
https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam/feature/control_kepad/OTA/version.json
https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam/feature/control_kepad/OTA/file.bin
```

Kiểm tra:

- `version.json` trả HTTP 200 và hiển thị version mới.
- `firmware_url` trong JSON đúng tuyệt đối với URL `file.bin` phía trên.
- `file.bin` tải được, không trả trang lỗi 404.
- Nhánh trong URL đúng `feature/control_kepad`.

Nên chờ Raw GitHub cập nhật ổn định rồi mới gửi command OTA. Nếu CDN tạm thời phục vụ manifest mới nhưng binary cũ, firmware sẽ từ chối vì image descriptor không khớp; không làm hỏng firmware đang chạy.

## 8. Bước 6 - Gửi command OTA

Publish command vào topic của Node:

```text
tbmq/keyboard/<node_id>/command
```

Ví dụ với `node_kbd_001`:

```text
tbmq/keyboard/node_kbd_001/command
```

Payload:

```json
{
  "ts": 1791604217,
  "version": "1.3",
  "request_id": "cmd-ota-1.5-node-kbd-001",
  "cmd": "OTA",
  "param": {}
}
```

Lưu ý: `version` trong command hiện là version protocol MQTT `1.3`, không phải version firmware cần tải. Version firmware được lấy từ `OTA/version.json`.

ACK tiếp nhận mong đợi:

```json
{
  "request_id": "cmd-ota-1.5-node-kbd-001",
  "ack_to": "OTA",
  "result": "ok",
  "description": "ota_accepted"
}
```

`ota_accepted` chỉ có nghĩa yêu cầu đã được đưa vào OTA task. Nó chưa khẳng định download và cài đặt đã thành công.

Không gửi liên tục nhiều command OTA. Queue OTA chỉ chứa một request; request mới trong lúc OTA đang chạy sẽ bị trả `busy`.

## 9. Bước 7 - Theo dõi log OTA

Luồng thành công điển hình:

```text
OTA_MANAGER: Kiem tra OTA manifest: https://raw.githubusercontent.com/...
OTA_MANAGER: OTA version current=1.4 remote=1.5
OTA_MANAGER: Tai OTA firmware: https://raw.githubusercontent.com/.../OTA/file.bin
esp_https_ota: Starting OTA...
OTA_MANAGER: Dang tai firmware 1.5 vao partition ota_1
OTA_MANAGER: OTA downloaded=... KiB
OTA_MANAGER: OTA firmware 1.5 da ghi va xac minh thanh cong
OTA_MANAGER: Khoi dong lai de chay firmware moi sau 2000 ms
```

Nếu `remote` bằng hoặc thấp hơn `current`:

```text
OTA_MANAGER: Firmware hien tai da moi nhat, khong can OTA
```

Thiết bị sẽ không download và không reboot trong trường hợp này.

## 10. Bước 8 - Xác nhận firmware mới và rollback

Sau reboot vào firmware mới, log mong đợi:

```text
OTA_MANAGER: Firmware moi dang PENDING_VERIFY, bat dau self-test
OTA_MANAGER: Self-test OTA thanh cong, firmware da duoc xac nhan
```

Self-test hiện chờ 10 giây rồi kiểm tra core config trong NVS. Nếu hợp lệ, firmware gọi API xác nhận app và hủy rollback.

Nếu firmware crash, reset hoặc tự đánh dấu không hợp lệ trước khi được xác nhận, bootloader sẽ quay lại OTA slot trước đó.

Sau khi xác nhận, kiểm tra:

```text
OTA_MANAGER: OTA manager started, current_version=1.5
```

Đồng thời kiểm tra heartbeat/telemetry đã gửi field `version` mới.

Không chủ động reset hoặc cắt nguồn trong 10 giây self-test đầu tiên nếu không phải đang kiểm thử rollback.

## 11. Các lỗi thường gặp

### HTTP 404

Log:

```text
esp_https_ota: File not found(404)
```

Nguyên nhân thường gặp:

- Chưa push `OTA/file.bin`.
- Push nhầm nhánh.
- `OTA_RELEASE_BRANCH` khác nhánh chứa file.
- Sai chữ hoa/thường trong tên thư mục hoặc tên file.
- Repository private.

Khắc phục: mở trực tiếp cả hai Raw URL và chỉ gửi lại OTA khi cả hai đều trả HTTP 200.

### `ESP_ERR_INVALID_VERSION`

Log:

```text
Image descriptor khong khop: project=KEYBOARD_PHUONG_NAM version=...
```

Nguyên nhân: `version.json` mới nhưng `file.bin` vẫn chứa version cũ, hoặc binary thuộc project khác.

Khắc phục:

```powershell
idf.py build
python tools/prepare_ota.py --branch feature/control_kepad
git add CMakeLists.txt OTA/file.bin OTA/version.json
git commit -m "fix: rebuild OTA image"
git push origin feature/control_kepad
```

Không sửa manifest bằng tay để chữa lỗi này.

### `ESP_ERR_INVALID_RESPONSE`

Nguyên nhân thường gặp:

- Manifest không trả HTTP 200.
- JSON sai cú pháp hoặc thiếu `version`/`firmware_url`.
- `firmware_url` không khớp URL được firmware cho phép.
- Raw GitHub/CDN đang trả nội dung chưa đồng bộ hoặc response rỗng.

Khắc phục: kiểm tra Raw manifest, đợi GitHub cập nhật rồi thử lại. Không gửi command liên tục mỗi vài giây.

### Version mới nhưng thiết bị báo đã mới nhất

Kiểm tra `PROJECT_VER`, version nhúng trong binary và manifest. Firmware chỉ tải khi:

```text
remote_version > current_version
```

### Thiết bị không nhận command OTA

Kiểm tra:

- Wi-Fi đã có IP.
- MQTT có log `MQTT ready, command topic da subscribe`.
- Topic đúng `node_id` trong NVS của thiết bị.
- Payload dùng `cmd` viết hoa chính xác là `OTA`.
- `param` là object rỗng `{}`.
- `request_id` chưa được sử dụng.
- Protocol `version` đúng `1.3`.

## 12. Checklist trước khi phát hành

```text
[ ] Đã tăng PROJECT_VER
[ ] Đã build thành công
[ ] Binary nhỏ hơn OTA partition 3 MB
[ ] Đã chạy prepare_ota.py với đúng branch
[ ] version.json và file.bin cùng version
[ ] Đã commit/push cả file.bin và version.json
[ ] Raw version.json trả HTTP 200
[ ] Raw file.bin trả HTTP 200
[ ] firmware_url khớp tuyệt đối với cấu hình firmware
[ ] Thiết bị có Wi-Fi và MQTT ready
[ ] Đã lưu request_id riêng cho lần OTA
[ ] Đã theo dõi log download đến khi reboot
[ ] Đã thấy PENDING_VERIFY và self-test thành công
[ ] Heartbeat/telemetry báo version mới
```

## 13. Quy trình rút gọn

```powershell
# 1. Tăng PROJECT_VER trong CMakeLists.txt
idf.py build

# 2. Tạo binary và manifest OTA cùng version
python tools/prepare_ota.py --branch feature/control_kepad

# 3. Kiểm tra và phát hành
git status --short
git add CMakeLists.txt OTA/file.bin OTA/version.json
git commit -m "release: firmware <version>"
git push origin feature/control_kepad

# 4. Kiểm tra hai Raw URL, sau đó mới gửi MQTT cmd=OTA
```

Thứ tự an toàn cần nhớ:

```text
Tăng version -> Build -> Prepare OTA -> Push binary + manifest
-> Kiểm tra Raw GitHub -> Gửi command OTA -> Theo dõi self-test
```
