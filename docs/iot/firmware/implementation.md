# Firmware Implementation and MQTT Contract

## 1. Tổng quan

ESP32-S3 đóng vai trò cầu nối giữa bàn phím/màn hình của bộ điều khiển bơm và ThingsBoard TBMQ:

- đọc phím vật lý và có thể phát lại phím ảo;
- giải mã LED 7 đoạn để lấy giao dịch và xác nhận command;
- gửi telemetry/ACK;
- nhận command đúng namespace của Node;
- giữ cấu hình và giao dịch pending trong NVS.

Nguồn runtime của cấu hình thiết bị là NVS `device_cfg/core_cfg`. `main/device_config.h` chỉ là factory seed cho lần boot đầu tiên. Không sao chép `NODE_ID`, broker hoặc credential sang module khác.

## 2. Định danh và kết nối MQTT

`node_id` phải có dạng `node_kbd_NNN`, với `NNN` từ `001` đến `999`. MQTT Client ID dùng chính `node_id` đã load từ NVS.

| Thuộc tính | Giá trị code hiện tại |
|---|---|
| Broker host | `core_cfg.mqtt_broker_host` |
| Port | `core_cfg.mqtt_broker_port`; factory seed hiện là `1883` |
| Transport | `MQTT_TRANSPORT_OVER_TCP` |
| Username/password | `core_cfg.mqtt_username`, `core_cfg.mqtt_password` |
| QoS | 1 |
| Retain | false |
| Keep alive | 60 giây |
| Clean session | true |

Production cần chuyển sang TLS và không lưu secret trực tiếp trong repository.

## 3. MQTT Topics

| Topic | Publisher | Subscriber | QoS | Retain |
|---|---|---|---:|---|
| `tbmq/keyboard/<node_id>/telemetry` | Node | Server | 1 | false |
| `tbmq/keyboard/<node_id>/command` | Server | Node | 1 | false |
| `tbmq/keyboard/<node_id>/ack` | Node | Server | 1 | false |
| `tbmq/keyboard/<node_id>/event` | Node | Server | 1 | false |

Node không subscribe wildcard. `MQTT_READY` chỉ được set sau khi broker xác nhận subscribe command topic.

## 4. Command envelope

Payload command thông thường:

```json
{
  "ts": 1788680965,
  "version": "1.3",
  "request_id": "cmd-20260906-90dcd695e655",
  "cmd": "get_totalizer",
  "param": {}
}
```

| Field | Kiểu | Bắt buộc | Validation |
|---|---|---:|---|
| `ts` | integer | có | không âm; riêng `reset_totalizer` hiện cho phép thiếu |
| `version` | string | có | phải bằng `1.3` |
| `request_id` | string | có | 1..64 ký tự |
| `cmd` | string | có | 1..48 ký tự |
| `param` | object | có | schema phụ thuộc command |

MQTT callback chỉ copy payload vào queue dài 8. `mqtt_command` task mới parse và thực thi. Payload tối đa 768 byte.

Firmware nhớ 16 `request_id` gần nhất trong RAM. Request trùng trả `rejected/duplicate_request`. Lịch sử này mất khi reboot.

## 5. ACK contract

ACK chung:

```json
{
  "ts": 1788681000,
  "version": "1.3",
  "request_id": "cmd-20260906-90dcd695e655",
  "ack_to": "get_totalizer",
  "result": "ok",
  "description": "totalizer_read_success",
  "reported": {
    "total_amount_vnd": 125400000,
    "total_volume_l": 5016.2
  },
  "time_device": "06/09/2026 10:30:00"
}
```

ACK phụ thuộc time đã sync vì builder dùng `time_manager_get_snapshot()`. `reported` luôn là object, có thể rỗng ở ACK lỗi.

Các `result` hiện dùng: `ok`, `error`, `rejected`.

| Description | Ý nghĩa |
|---|---|
| `invalid_param` | Envelope hoặc param sai |
| `unsupported_command` | Chưa có implementation |
| `duplicate_request` | Trùng request trong lịch sử RAM |
| `verification_failed` | Thao tác xong nhưng LED/readback không đúng |
| `storage_error` | Ghi NVS thất bại |
| `internal_error` | Trạng thái module không hợp lệ |
| `reported_mismatch` | Readback mapping khác dữ liệu yêu cầu |
| `mapping_limit_reached` | Không còn vị trí mapping |

## 6. Command đã hỗ trợ

### 6.1 `set_hash_key_lock`

```json
{"param":{"locked":true}}
```

- Lưu NVS `device_cfg/hash_lock`.
- `true`: bỏ qua phím `#` vật lý; phím ảo vẫn hoạt động.
- ACK thành công: `hash_key_lock_updated`, reported `hash_key_locked`.

### 6.2 `set_price_edit_lock`

```json
{"param":{"locked":true}}
```

- Lưu NVS `device_cfg/price_lock` và readback trước khi ACK `ok`.
- Chưa nối vào logic khóa thao tác đổi giá.
- Giữ trạng thái qua reboot và OTA app.
- ACK thành công: `price_edit_lock_updated`, reported `price_edit_locked`.

### 6.3 `set_unit_price`

```json
{"param":{"unit_price":31000}}
```

- Khoảng hợp lệ: 1..999999, integer.
- ACK `unit_price_accepted` được gửi trước khi thực thi.
- Chuỗi phím: `#44504EP` + giá + `E`.
- Xác nhận LED: hàng 1 là `E0`, hàng 3 đúng giá.
- Thành công mới gửi `C`; retry tối đa 3 lần.
- Khoảng phím mặc định 500 ms.

Lưu ý: code hiện không gửi ACK cuối thứ hai nếu thao tác thực tế thất bại sau ACK accepted.

### 6.4 `get_totalizer`

`param` phải rỗng.

Luồng tổng tiền:

```text
C#7733 -> E -> chờ P20 -> decode hàng 2 -> C
```

Luồng tổng lít:

```text
C#44504ET -> chờ 1 giây -> decode màn hình -> C
```

Mỗi phần retry tối đa 3 lần. Cả hai thành công mới:

- ACK `totalizer_read_success` với hai giá trị trong `reported`;
- publish thêm totalizer telemetry.

### 6.5 `close_shift`

`param` phải rỗng. Reuse `read_totalizer()` giống `get_totalizer` nhưng chỉ ACK:

- description `shift_closed`;
- reported có `total_amount_vnd`, `total_volume_l`.

Backend chịu trách nhiệm bảo đảm chốt ca đúng một lần.

### 6.6 `reset_totalizer`

```json
{"param":{"confirm":true}}
```

Tổng tiền:

```text
C#7733 -> E -> verify P20 -> 000 -> verify E0 -> C
```

Tổng lít:

```text
C#845443E -> verify P08 -> 0 -> verify hàng 2 = 0
-> E -> verify E0 -> C
```

Mỗi phần retry 3 lần. ACK `totalizer_reset` chỉ khi cả hai thành công, reported bằng 0.

### 6.7 `set_shortcut_mapping`

Mỗi command cập nhật đúng một slot từ 1 đến 10:

```json
{
  "param": {
    "physical_key_1": "C#7733E",
    "shortcut_key_1": "P123"
  }
}
```

- Hai field phải cùng suffix slot.
- Ký tự cho phép: `0-9`, `#`, `$`, `C`, `E`, `L`, `P`, `T`, `V`.
- `shortcut_key`: tối đa 16 ký tự.
- `physical_key`: tối đa 32 ký tự.
- Không cho trùng shortcut ở slot khác.
- Ghi NVS, readback và kiểm tra revision trước ACK.
- ACK `shortcut_mapping_updated`, reported chứa `mapping` và `revision`.

Khi người dùng nhập shortcut vật lý, sequence output được đưa qua queue. Phím ảo không đi lại qua bộ nhận dạng shortcut nên không tạo vòng lặp.

### 6.8 `send_calibration_command`

Mỗi command cập nhật một slot từ 1 đến 5:

```json
{
  "param": {
    "name_3": "P555",
    "raw_command_3": "2530"
  }
}
```

- `name`: shortcut hợp lệ, tối đa 16 ký tự.
- `raw_command`: chỉ số, dài 1..6.
- Gửi `C#122973E`, verify `P06`.
- Chờ 1000 ms rồi nhập raw command.
- Verify hàng 2 đúng raw command.
- Gửi `E`, verify hàng 1 `E0` và hàng 2 vẫn đúng.
- Gửi `C` để thoát.
- Retry tối đa 3 lần.
- Chỉ sau verify thành công mới lưu mapping và `mode_calibration=name`.
- ACK `calibration_command_applied`, reported gồm `name`, `command_echo`, `mode_calibration`.

Calibration shortcut local reuse chính hàm `execute_calibration_command()` và không phát MQTT ACK.

### 6.9 `OTA`

```json
{
  "ts": 1789369157,
  "version": "1.3",
  "request_id": "cmd-ota-001",
  "cmd": "OTA",
  "param": {}
}
```

- Command name phân biệt hoa/thường và phải đúng `OTA`.
- Handler đưa request vào queue rồi ACK `ok/ota_accepted`; không tải firmware trong MQTT callback/task.
- Nếu đã có OTA chạy hoặc chờ: ACK `rejected/busy`.
- Payload sai hoặc `param` không rỗng: ACK `error/invalid_param`.
- Manifest: `OTA/version.json`, bắt buộc có `version` và `firmware_url`.
- Chỉ tải khi remote semantic version lớn hơn `PROJECT_VER` hiện tại.
- Image phải có cùng `project_name` và app version phải đúng version manifest.
- HTTPS xác minh server bằng ESP x509 Certificate Bundle.
- GitHub repository phải public; triển khai private cần cơ chế credential riêng.

Ví dụ manifest:

```json
{
  "version": "1.0.1",
  "firmware_url": "https://raw.githubusercontent.com/kenhkythuat/keyboard_phuongnam/<OTA_RELEASE_BRANCH>/OTA/file.bin",
  "size": 1161376
}
```

Rollback được bật bằng `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. Firmware mới chờ 10 giây, kiểm tra core config NVS rồi gọi `esp_ota_mark_app_valid_cancel_rollback()`. Nếu self-test lỗi, firmware gọi rollback và reboot.

## 7. Command khai báo nhưng chưa triển khai

Các tên sau xuất hiện trong danh sách command nhưng chưa có handler:

- `set_reset_shortcut`
- `delete_shortcut_mapping`
- `get_reported_state`

Chúng hiện trả `result=rejected`, `description=unsupported_command`.

## 8. Transaction telemetry

Topic: `tbmq/keyboard/<node_id>/telemetry`.

Field `version` cua moi telemetry lay tu app descriptor qua
`firmware_version_get()`. Day cung la version OTA duoc sinh tu `PROJECT_VER`;
khong con hard-code `1.3` trong telemetry. Version `1.3` chi con la schema cua
command/ACK.

```json
{
  "ts": 1789369621,
  "version": "1.0.5",
  "data": {
    "unit_price": 25000,
    "amount_vnd": 7850,
    "volume_ml": 314,
    "command_code": "P555",
    "time_device": "14/09/2026 14:07:01",
    "is_buffered": false,
    "RSSI": -30
  }
}
```

| Field | Kiểu | Đơn vị / quy tắc |
|---|---|---|
| `ts` | integer 64-bit | Unix UTC giây |
| `version` | string | firmware `PROJECT_VER`, cung gia tri voi OTA app version |
| `unit_price` | number | VNĐ/L |
| `amount_vnd` | number | VNĐ |
| `volume_ml` | number | mL |
| `command_code` | string | mode calibration tại lúc bắt đầu lượt bơm |
| `time_device` | string | `DD/MM/YYYY HH:mm:ss`, Asia/Ho_Chi_Minh |
| `is_buffered` | boolean | `true` khi đọc từ NVS pending |
| `RSSI` | integer | dBm |

Validation trước publish:

- ba giá trị giao dịch phải lớn hơn 0;
- `command_code` hợp lệ;
- chênh lệch tiền so với `unit_price * volume_ml / 1000` không quá 200 VNĐ;
- time valid, lấy được RSSI và MQTT ready.

Nếu chưa đủ điều kiện mạng/time, lưu tối đa 32 giao dịch vào NVS. Bản ghi giữ `command_code` gốc nhưng hiện chưa giữ timestamp gốc; thời gian được lấy lúc gửi lại.

## 9. Totalizer telemetry

Được gửi sau `get_totalizer` thành công:

```json
{
  "ts": 1786523009,
  "version": "1.0.5",
  "data": {
    "total_amount_vnd": 125400000,
    "total_volume_l": 5016.2,
    "time_device": "13/08/2026 10:23:29"
  }
}
```

## 10. Heartbeat telemetry

Chu kỳ 60 giây:

```json
{
  "ts": 1786522928,
  "version": "1.0.5",
  "keep_alive": 1,
  "RSSI": -75,
  "data": {
    "unit_price": 25000
  }
}
```

Heartbeat bị bỏ qua nếu time chưa valid, RSSI lỗi, MQTT chưa ready hoặc chưa đọc được `unit_price` hợp lệ.

## 11. Wi-Fi Config Portal

Giữ IO0 active-low 7 giây, debounce 50 ms. Firmware mở AP từ `WIFI_CONFIG_AP_SSID`, tối đa 4 client, channel 1.

| Method | Endpoint | Chức năng |
|---|---|---|
| GET | `/` | Form nhập SSID/password |
| POST | `/save` | Validate và queue credential thử nghiệm |
| GET | `/status` | Trạng thái portal/kết nối |

SSID tối đa 32 byte; password tối đa 64 byte ở form. Credential cũ chỉ bị thay sau khi credential mới nhận được IP và ghi/readback NVS thành công.

## 12. Time manager

- SNTP non-blocking, server `pool.ntp.org`.
- Sync khi STA có IP và re-sync mỗi giờ.
- TZ chuẩn qua `localtime_r()`, không cộng 7 thủ công.
- `time_manager_get_snapshot()` là API nên dùng khi một payload cần cả `ts` và `time_device`.

## 13. NVS layout logic

| Namespace/key | Kiểu | Giới hạn |
|---|---|---|
| `wifi_cfg/credentials` | blob có magic/version | 1 credential |
| `device_cfg/core_cfg` | versioned blob | node ID + MQTT host/port/credential |
| `device_cfg/hash_lock` | u8 | boolean |
| `device_cfg/price_lock` | u8 | boolean |
| `device_cfg/cal_mode` | string | 16 ký tự |
| `device_cfg/shortcuts` | versioned blob | 10 slot + revision |
| `device_cfg/calib_map` | versioned blob | 5 slot |
| `pump_tx/queue` | versioned blob | 32 giao dịch |

Các blob có magic/version và một số luồng migration từ version cũ. `core_cfg` được seed từ `device_config.h` nếu chưa tồn tại hoặc không hợp lệ. MQTT không log password.

Quy trình xuất xưởng:

1. Đặt factory seed riêng cho board trong `device_config.h`, đặc biệt `NODE_ID` duy nhất.
2. Build và flash firmware lần đầu.
3. Boot đầu seed `core_cfg`, Wi-Fi, lock, mode và bảng mapping rỗng vào NVS, sau đó readback.
4. Mọi thay đổi qua API/command phải commit và readback NVS trước khi báo thành công.
5. Reboot hoặc OTA app load lại NVS; factory seed mới trong binary không ghi đè cấu hình đã provision.

## 14. Phát hành OTA

1. Tăng `PROJECT_VER` trong root `CMakeLists.txt`.
2. Chạy `idf.py build`.
3. Chạy `python tools/prepare_ota.py --branch <OTA_RELEASE_BRANCH>`.
4. Kiểm tra version trong `OTA/version.json` trùng app version vừa build.
5. Commit/push `OTA/file.bin` và `OTA/version.json` lên nhánh `main` cùng một lần.
6. Gửi command `OTA` và theo dõi log `OTA_MANAGER` qua lần reboot/self-test.

Không dùng `git@github.com:...` trong firmware; ESP32 tải bằng HTTPS Raw GitHub.

## 15. Điểm mở rộng khuyến nghị

- Bổ sung chữ ký firmware/Secure Boot để chống image bị thay thế nếu tài khoản GitHub bị xâm nhập.
- Gửi ACK kết quả cuối cho `set_unit_price`, thay vì chỉ ACK accepted.
- Persist dedup request nếu backend cần idempotency qua reboot.
- Lưu timestamp gốc cho buffered transaction.
- Chuyển MQTT TCP sang TLS và đưa secret ra khỏi source.
