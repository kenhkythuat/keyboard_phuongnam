# KEYBOARD_PHUONG_NAM

Firmware ESP-IDF cho ESP32-S3 dùng để:

- đọc bàn phím ma trận 5x5;
- phát mã phím ảo 5 bit sang bộ điều khiển ngoài;
- bắt và giải mã bus LED 7 đoạn MBI5026, kích thước 3 hàng x 6 cột;
- nhận biết một lượt bơm, kiểm tra dữ liệu và gửi MQTT telemetry;
- nhận MQTT command, thao tác phím ảo và xác nhận kết quả bằng dữ liệu LED;
- cấu hình Wi-Fi bằng SoftAP/Web Portal;
- đồng bộ thời gian SNTP và lưu dữ liệu cần thiết trong NVS.

Tài liệu này mô tả **code hiện tại trong repository**, không phải đặc tả dự kiến.

## Đọc tài liệu theo thứ tự

1. README này: tổng quan phần cứng, module và luồng chính.
2. [MQTT và triển khai](docs/iot/firmware/implementation.md): topic, payload, command, NVS và API.
3. [Sơ đồ luồng](docs/iot/firmware/flow.md): boot, giao dịch, command, Wi-Fi và time sync.
4. [Lỗi và trường hợp biên](docs/iot/firmware/exceptions.md): validation, retry và cách chẩn đoán.

File [FIRMWARE_ARCHITECTURE.md](FIRMWARE_ARCHITECTURE.md) là tài liệu cũ và có một số thông tin không còn đúng. Khi có khác biệt, ưu tiên README và source hiện tại.

## Nền tảng

| Thành phần | Giá trị hiện tại |
|---|---|
| MCU | ESP32-S3 |
| Framework | ESP-IDF 5.3.x |
| RTOS | FreeRTOS |
| Flash | 8 MB |
| MQTT payload | JSON UTF-8; command/ACK schema `1.3`, telemetry version = firmware `PROJECT_VER` |
| Timezone thiết bị | `Asia/Ho_Chi_Minh` qua TZ `ICT-7` |
| NTP server | `pool.ntp.org` |
| Node mặc định | `node_kbd_001` |

## Cấu trúc module

| File | Trách nhiệm chính |
|---|---|
| `main/main.c` | Khởi động hệ thống, scan keypad, phát phím ảo, shortcut local, P88 và hiển thị trạng thái Wi-Fi |
| `main/pump_data_sniffer.c` | SPI slave capture bus ngoài, ghép 6 cột và giải mã LED 3x6 |
| `main/pump_transaction_filter.c` | Parse tiền/lít/đơn giá, nhận biết lượt bơm, validate và gửi telemetry |
| `main/pump_transaction_store.c` | Hàng đợi giao dịch pending trong NVS, tối đa 32 giao dịch |
| `main/control_display_led.c` | Điều khiển MBI5026 output và chế độ Virtual LED |
| `main/mqtt_manager.c` | Kết nối broker, topic, subscribe, publish và reconnect |
| `main/mqtt_command_handler.c` | Queue command, validate JSON, thực thi command và tạo ACK |
| `main/wifi_manager.c` | Wi-Fi STA, reconnect, giữ IO0 để vào Config Mode và thử credential mới |
| `main/wifi_config_portal.c` | HTTP Web Portal nhập SSID/password |
| `main/time_manager.c` | SNTP, system time, `ts` và `time_device` |
| `main/device_settings.c` | Cấu hình persistent: khóa `#`, shortcut, calibration và mode calibration |
| `main/telemetry_heartbeat.c` | Heartbeat MQTT mỗi 60 giây |
| `main/ota_manager.c` | Kiểm tra version, HTTPS OTA, self-test và rollback |
| `main/device_config.h` | Node ID, MQTT, Wi-Fi mặc định và cấu hình portal |

## Sơ đồ tổng quát

```text
Keypad 5x5 ──> keypad_scan_task ──> D0..D4 virtual key output
       │                │
       │                ├──> shortcut mapping ──> virtual key sequence
       │                ├──> calibration mapping ──> calibration flow
       │                └──> P88 ──> read_totalizer ──> Virtual LED
       │
External MBI bus ──> SPI capture ──> LED decoder 3x6
                                      │
                                      ├──> transaction filter
                                      │       ├──> validation
                                      │       ├──> NVS pending queue
                                      │       └──> MQTT telemetry
                                      │
                                      └──> command verification/readback

Wi-Fi STA ──> SNTP time ──> MQTT ──> command queue / ACK / telemetry
     └──> IO0 hold 7 s ──> SoftAP + HTTP portal
```

## Kết nối GPIO

### Keypad 5x5

Các hàng được phát mức `1` lần lượt; cột dùng input pulldown.

| Tín hiệu | GPIO | Tín hiệu | GPIO |
|---|---:|---|---:|
| ROW1 | 19 | COL1 | 21 |
| ROW2 | 20 | COL2 | 14 |
| ROW3 | 46 | COL3 | 13 |
| ROW4 | 9 | COL4 | 12 |
| ROW5 | 10 | COL5 | 11 |

Mapping hiện tại:

| | COL1 | COL2 | COL3 | COL4 | COL5 |
|---|---|---|---|---|---|
| ROW1 | `#` | `P` | `7` | `8` | `9` |
| ROW2 | `E` | `V` | `4` | `5` | `6` |
| ROW3 | `C` | `T` | `1` | `2` | `3` |
| ROW4 | `F4` | `F5` | `F6` | `$` | `0` |
| ROW5 | `F1` | `F2` | `F3` | `L` | không dùng |

### Phím ảo 5 bit

| Bit | GPIO |
|---|---:|
| D0 | 16 |
| D1 | 8 |
| D2 | 18 |
| D3 | 17 |
| D4 | 7 |

`KEY_NONE = 0`. Phím vật lý và các command đều reuse cùng cơ chế output này. Chuỗi phím tự động dùng mutex/queue để không chồng lên nhau.

### Sniffer dữ liệu LED đầu vào

| Chức năng | GPIO |
|---|---:|
| OE input | 4 |
| LE/CS input | 5 |
| SDI/MOSI input | 6 |
| CLK input | 15 |

Bus được capture bằng `SPI2_HOST`, SPI slave mode 1. Một giao dịch chuẩn có 192 bit, tương ứng 6 block x 32 bit.

### MBI5026 LED output

| Chức năng | GPIO |
|---|---:|
| SDI | 37 |
| OE | 38 |
| CLK | 36 |
| LE | 35 |
| ENABLE_VIRTUAL_LED | 39 |

- `IO39 = 0`: mạch ngoài điều khiển LED.
- `IO39 = 1`: ESP32 giành quyền hiển thị Virtual LED.

### Wi-Fi Config

| Chức năng | GPIO |
|---|---:|
| Nút vào Config Mode | IO0, active-low |

Nhấn giữ IO0 ổn định hơn 7 giây để mở SoftAP và Web Portal.

## Boot flow

`app_main()` khởi động theo thứ tự chính:

1. Đặt IO39 về `0`.
2. Khởi động `wifi_manager` và NVS.
3. Load `device_settings`.
4. Khởi động `time_manager`.
5. Khởi động `mqtt_manager`.
6. Khởi động heartbeat.
7. Khởi động sniffer và transaction filter.
8. Khởi tạo keypad và virtual key output.
9. Tạo task shortcut, MQTT command, P88, Wi-Fi status LED và keypad scan.

Các module mạng đều event-driven. `app_main()` không chờ Wi-Fi, SNTP hoặc MQTT.

## Luồng giải mã LED

```text
SPI transaction 192 bit
  -> decode 6 block/cột
  -> mỗi cột phải ổn định 2 lần
  -> đủ 6 cột
  -> toàn màn hình phải giống nhau 2 lần
  -> nếu nội dung khác màn hình trước: publish snapshot mới
```

`pump_data_sniffer` chỉ gửi màn hình sang transaction filter khi nội dung đã xác nhận và thay đổi. Dữ liệu SPI vẫn được capture khi màn hình đứng yên.

Log `STAT` được điều khiển bởi:

```c
#define ENABLE_PERIODIC_REPORT_LOG 0
#define REPORT_PERIOD_MS 500U
```

Khi bật periodic report, các counter tăng chứng minh bus vẫn đang nhận dữ liệu. Log COL/LED chi tiết mặc định chỉ xuất hiện khi màn hình thay đổi.

## Luồng giao dịch bơm

Ba hàng LED được hiểu là:

| Hàng | Ý nghĩa | Đơn vị nội bộ |
|---|---|---|
| Hàng 1 | Số tiền | VNĐ |
| Hàng 2 | Thể tích | mL sau khi bỏ dấu thập phân hiển thị |
| Hàng 3 | Đơn giá | VNĐ/L |

Điều kiện bắt đầu:

```text
amount_vnd = 0
volume_ml = 0
unit_price > 0
```

Sau đó firmware theo dõi mẫu dương:

- nếu có nhiều mẫu thì xác nhận tiền/lít tăng không giảm;
- nếu lượt bơm rất nhanh chỉ có một mẫu kết quả, mẫu đó vẫn được nhận nếu khác màn hình dương trước khi về `0/0`;
- nếu màn hình quay lại đúng giao dịch cũ thì không phát lại;
- kết quả phải đứng yên 10 giây;
- công thức kiểm tra:

```c
expected_amount_vnd = ((float)unit_price * volume_ml) / 1000.0f;
difference_vnd = fabsf(amount_vnd - expected_amount_vnd);
```

Sai số hiện tại cho phép `±200 VNĐ`.

Khi hợp lệ, transaction lấy một snapshot thời gian, RSSI và `mode_calibration`, rồi gửi telemetry. Nếu time/network/MQTT chưa sẵn sàng, transaction được lưu NVS và gửi lại sau với `is_buffered=true`.

## MQTT tóm tắt

Thông số lấy từ `main/device_config.h` và `mqtt_manager.c`:

| Thuộc tính | Hiện tại |
|---|---|
| Transport | MQTT TCP thường, chưa TLS |
| Port | `1883` |
| Client ID | `NODE_ID` |
| QoS | `1` |
| Retain | `false` |
| Keep alive | `60` giây |
| Clean session | bật |
| Reconnect | `1, 2, 4, ... 60` giây + jitter tối đa 500 ms |

Topics được tạo động từ `NODE_ID`:

```text
tbmq/keyboard/<node_id>/telemetry
tbmq/keyboard/<node_id>/command
tbmq/keyboard/<node_id>/ack
tbmq/keyboard/<node_id>/event
```

Node chỉ subscribe topic `command` của chính nó. Chi tiết payload và command nằm trong [implementation.md](docs/iot/firmware/implementation.md).

## Wi-Fi và Web Portal

- Khi boot, firmware ưu tiên credential trong NVS `wifi_cfg/credentials`.
- Nếu chưa có, credential factory trong `device_config.h` được seed vào NVS và dùng từ đó về sau.
- Mất kết nối: backoff từ 1 đến 30 giây, không reset MCU.
- Giữ IO0 hơn 7 giây: chuyển sang `WIFI_MODE_APSTA`, mở AP cấu hình.
- Endpoint: `GET /`, `POST /save`, `GET /status`.
- Credential mới được thử trước; chỉ lưu NVS sau khi nhận `GOT_IP`.
- Thành công hiển thị `donE`, thất bại hiển thị `FAiL`, Config Mode hiển thị `CONFIG` bằng Virtual LED.

Không log password Wi-Fi.

## Time sync

- SNTP bắt đầu khi có `IP_EVENT_STA_GOT_IP`.
- Re-sync mỗi 1 giờ.
- Khi mất mạng, system time tiếp tục chạy.
- `time_manager_get_snapshot()` lấy `ts` và `time_device` từ cùng một `time_t`.
- Chưa sync thì API trả lỗi, không tạo timestamp giả.

## NVS và dữ liệu bền vững

| Namespace | Key | Nội dung |
|---|---|---|
| `wifi_cfg` | `credentials` | SSID/password đã xác nhận |
| `device_cfg` | `core_cfg` | `node_id`, MQTT host/port, username/password |
| `device_cfg` | `hash_lock` | Trạng thái khóa phím `#` |
| `device_cfg` | `price_lock` | Trạng thái khóa sửa giá |
| `device_cfg` | `cal_mode` | `mode_calibration`, mặc định `P1E` |
| `device_cfg` | `cal_restore` | Mode trước khi chuyển tạm sang calibration slot 1 |
| `device_cfg` | `shortcuts` | 10 shortcut mapping + revision |
| `device_cfg` | `calib_map` | 5 calibration mapping |
| `pump_tx` | `queue` | Tối đa 32 giao dịch pending |

`main/device_config.h` chỉ chứa giá trị seed dùng khi xuất xưởng. Lần boot đầu tiên ghi và readback cấu hình lõi vào NVS. Các lần boot sau, MQTT luôn lấy cấu hình từ NVS; thay đổi macro trong một firmware OTA không ghi đè thiết bị đã provision.

Các API `device_settings_get_core_config()` và `device_settings_set_core_config()` là điểm đọc/ghi duy nhất cho cấu hình lõi. API ghi chỉ cập nhật RAM sau khi commit và readback NVS thành công. Cấu hình MQTT mới có hiệu lực sau reboot để tránh tạo nhiều MQTT client.

## Partition và OTA

Flash 8 MB đã có hai slot app 3 MB:

```text
nvs | otadata | phy_init | ota_0 | ota_1 | storage | coredump
```

Partition `nvs` nằm ngoài `ota_0` và `ota_1`, vì vậy OTA chỉ cập nhật app sẽ giữ cấu hình. `erase_flash`, xóa partition NVS hoặc thay partition table làm mất vùng `0x9000..0xEFFF` vẫn có thể xóa cấu hình.

Command `OTA` với `param={}` được ACK ngay bằng `ota_accepted`, sau đó `ota_manager` chạy trong task riêng:

1. Đọc `OTA/version.json` qua HTTPS Raw GitHub.
2. So sánh version dạng `major.minor.patch` với `PROJECT_VER` hiện tại.
3. Nếu version mới hơn, tải `OTA/file.bin` vào OTA slot còn lại.
4. Kiểm tra project/version trong image descriptor rồi đổi boot partition và reboot.
5. Firmware mới ở trạng thái `PENDING_VERIFY`; sau 10 giây self-test NVS thành công sẽ tự xác nhận. Reset/crash trước khi xác nhận sẽ rollback.

Repository phải public để ESP32 truy cập Raw GitHub. URL SSH `git@github.com:...` chỉ dùng bởi Git trên máy phát triển, không phải URL download của thiết bị.

## Build và flash

Mở ESP-IDF terminal rồi chạy:

```powershell
idf.py set-target esp32s3
idf.py build
python tools/prepare_ota.py --branch feature/control_kepad
idf.py -p COMx flash monitor
```

Mỗi release phải tăng `PROJECT_VER` trong `CMakeLists.txt`, build lại, chạy `tools/prepare_ota.py --branch <OTA_RELEASE_BRANCH>`, rồi commit/push đồng thời `OTA/file.bin` và `OTA/version.json` lên đúng nhánh cấu hình tại `OTA_RELEASE_BRANCH` trong `main/device_config.h`. Không publish manifest mới trước binary tương ứng.

Không commit credential production vào source. Trước khi nhân bản board phải đổi `NODE_ID` thành giá trị duy nhất trong khoảng `node_kbd_001` đến `node_kbd_999`.

## Checklist phát triển tính năng mới

1. Xác định nguồn trigger: keypad, MQTT, timer hay LED decoder.
2. Không xử lý dài trong callback MQTT/Wi-Fi; đưa vào queue/task.
3. Reuse `virtual_key_output_run_sequence()` cho thao tác phím.
4. Khi xác nhận màn hình, lấy `generation` trước khi phát phím và chỉ chấp nhận frame mới.
5. Dùng `time_manager_get_snapshot()` để tạo `ts` và `time_device` cùng thời điểm.
6. Cấu hình cần qua reboot phải dùng NVS, có readback verification.
7. ACK `ok` chỉ sau khi trạng thái thực tế đã được xác nhận.
8. Cập nhật cả README và tài liệu trong `docs/iot/firmware/`.
