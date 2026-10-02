# Error Handling and Edge Cases

## 1. MQTT và command

| Điều kiện | Phát hiện | Xử lý hiện tại | ACK/log | Ảnh hưởng trạng thái |
|---|---|---|---|---|
| Payload rỗng hoặc >768 byte | MQTT callback | Bỏ payload | `Command payload length khong hop le` | Không đổi |
| Command queue đầy | `xQueueSend` thất bại | Bỏ payload mới | `Command queue day` | Không ACK |
| JSON sai | cJSON parse lỗi | Bỏ command | `Command JSON khong hop le` | Không đổi |
| Thiếu `request_id` hoặc `cmd` | Validate field | Không thể tạo ACK | Log lỗi | Không đổi |
| Version khác `1.3` | Envelope validation | Reject param | `error/invalid_param` | Không đổi |
| Command chưa hỗ trợ | Không có handler | Reject | `rejected/unsupported_command` | Không đổi |
| Trùng `request_id` | Lịch sử 16 request RAM | Không thực thi lại | `rejected/duplicate_request` | Không đổi |
| MQTT mất kết nối | Event disconnected | Clear READY, hẹn reconnect | Log backoff | Task khác vẫn chạy |
| Wi-Fi mất | WIFI disconnect | Dừng MQTT reconnect timer; Wi-Fi tự reconnect | Log reason | Không reset MCU |
| ACK lúc time chưa valid | command task | Task chờ time valid | Không ACK cho đến khi ready | Command nằm trong queue/task |

Lưu ý: dedup không persistent. Sau reboot, cùng `request_id` có thể được xử lý lại.

## 2. LED decoder

| Điều kiện | Xử lý |
|---|---|
| Frame không phải 192 bit | Tăng counter `other`, không đưa vào display |
| Cột chưa lặp đủ 2 lần | Giữ candidate, chưa xác nhận |
| Chưa đủ 6 cột | Tiếp tục ghép; cột 1 blank có fallback sau khoảng 1 giây |
| Full display chưa lặp đủ 2 lần | Không publish snapshot |
| Màn hình giống lần trước | Vẫn nhận SPI nhưng không log/submit lại |
| Segment lạ trong vùng số giao dịch | Parse transaction thất bại và log vị trí |
| Ký tự `P`, `E` trong màn hình chức năng | Được command-specific decoder xử lý; không dùng parser giao dịch |

Các counter `spi_rx`, `decoded`, `bits192`, `valid_blocks` tăng là dấu hiệu đường capture vẫn hoạt động, ngay cả khi COL/LED không in lại.

## 3. Transaction filter

| Trường hợp | Hành vi |
|---|---|
| Boot khi đang hiển thị giao dịch cũ | Lưu làm idle baseline, không gửi |
| Màn hình về `0/0` rồi quay đúng số cũ | Xem là dữ liệu cũ được phục hồi, không gửi trùng |
| Lượt bơm nhanh chỉ có một mẫu kết quả | Chấp nhận nếu khác baseline trước `0/0`, sau đó chờ ổn định 10 giây |
| Boot đúng lúc màn hình `0/0`, không có baseline | Cần quan sát thêm bước tăng để tránh gửi nhầm |
| Tiền/lít giảm hoặc đơn giá đổi trong lượt | Reset progressive baseline và log cảnh báo |
| Dữ liệu chưa đứng yên 10 giây | Không finalize |
| Chênh công thức >200 VNĐ | Bỏ giao dịch, log `DU LIEU BOM KHONG HOP LE` |
| Time/RSSI/MQTT chưa sẵn sàng | Lưu NVS pending |
| Pending queue đầy hoặc NVS lỗi | Log lỗi; giao dịch mới có thể không được lưu |

Giao dịch invalid do công thức hiện không được retry và không gửi telemetry.

## 4. Totalizer và calibration

| Điều kiện | Xử lý |
|---|---|
| Frame LED cũ | So sánh `generation` với baseline và bỏ qua |
| Không thấy `P20`, `P08`, `P06` hoặc `E0` | Thoát bằng `C` và retry |
| Tổng tiền/tổng lít decode lỗi | Retry tối đa 3 lần |
| Một phần totalizer thất bại | Trả cả hai giá trị về 0, không publish telemetry hợp lệ |
| Raw calibration không khớp | Retry toàn bộ calibration tối đa 3 lần |
| Calibration thất bại | Không ghi mapping/mode mới |
| P88 đọc thất bại | Không hiển thị dữ liệu cũ; thoát trạng thái active |
| Giá trị P88 vượt 6 LED mỗi hàng | Log lỗi và không cắt chữ số |

`s_totalizer_mutex` ngăn get/reset/calibration chạy đồng thời.

## 5. NVS

| Điều kiện | Xử lý |
|---|---|
| NVS hết page hoặc version mới ở boot Wi-Fi | `nvs_flash_erase()` rồi init lại |
| Blob mapping sai magic/version | Bỏ blob và dùng bảng rỗng |
| Blob legacy hợp lệ | Migrate sang layout slot mới và commit |
| Mapping trùng shortcut ở slot khác | `ESP_ERR_INVALID_ARG`, ACK `invalid_param` |
| Ghi mapping xong readback sai | ACK `reported_mismatch` hoặc `verification_failed` |
| Mode calibration chưa lưu | Dùng mặc định `P1E` |

Cảnh báo: nhánh erase NVS khi init lỗi sẽ xóa Wi-Fi, settings và pending transaction vì chúng cùng nằm trong partition NVS.

## 6. Wi-Fi Config Portal

| Điều kiện | Xử lý |
|---|---|
| Nhấn IO0 ngắn | Không vào Config Mode |
| Giữ IO0 >7 giây | Trigger một lần cho lần giữ đó |
| SSID rỗng/quá dài | HTTP 400 |
| Body >320 byte | HTTP 400 |
| Credential mới sai | Giữ portal, phục hồi credential cũ nếu có, hiển thị FAiL |
| Credential mới có IP nhưng ghi NVS lỗi | Không áp dụng lâu dài, báo storage failure |
| Router mất nguồn | Reconnect 1..30 giây, không reset |

Password không được ghi ra log. AP password vẫn đang nằm trong firmware config và cần đổi trước production.

## 7. Time

| Điều kiện | Xử lý |
|---|---|
| Chưa SNTP sync | `time_manager_is_valid=false`, timestamp API trả lỗi/-1 |
| Mất Wi-Fi sau sync | System time tiếp tục chạy |
| Có IP lại | Yêu cầu sync lại |
| SNTP start lỗi | Log cảnh báo, không reset |

## 8. Bảo mật và production

Hiện trạng cần xử lý trước khi triển khai production:

- MQTT đang dùng TCP thường, không TLS.
- MQTT và Wi-Fi factory credential nằm trong header firmware.
- Secure Boot và Flash Encryption chưa bật.
- OTA chưa triển khai dù partition đã sẵn sàng.
- Bootloader rollback chưa bật.
- Web Portal chỉ phù hợp mạng cấu hình cục bộ, chưa có CSRF/session authentication.

## 9. Log chẩn đoán theo module

| TAG | Nội dung cần xem |
|---|---|
| `MBI_SNIFFER` | SPI counters, COL, LED rows, offset/assembly |
| `PUMP_FILTER` | Start/stop lượt bơm, progressive data, validation, telemetry |
| `PUMP_STORE` | Pending NVS queue |
| `MQTT_MANAGER` | Connect, subscribe, publish queue, reconnect |
| `MQTT_COMMAND` | Param, retry, verification và ACK |
| `WIFI_MANAGER` | STA/AP, reconnect, credential trial |
| `TIME_MANAGER` | SNTP request và sync |
| `TELEMETRY_HEARTBEAT` | Điều kiện heartbeat hoặc lý do bỏ qua |
| `KEY_OUTPUT` | Keypad, virtual key, shortcut, P88 và IO39 |

## 10. Test regression tối thiểu

1. Boot khi màn hình đang có giao dịch cũ: không gửi telemetry.
2. `0/0` -> một mẫu kết quả -> đứng 10 giây: gửi đúng một giao dịch.
3. `0/0` -> quay lại đúng giao dịch cũ: không gửi.
4. Bơm tăng qua nhiều bước: lấy mẫu cuối cùng.
5. Mất Wi-Fi/time khi hoàn tất: lưu NVS, reboot, gửi lại khi ready.
6. Gửi command trùng `request_id`: lần hai bị reject.
7. Mất router và có lại: Wi-Fi, SNTP, MQTT tự phục hồi.
8. Sai password portal: credential cũ không bị mất.
9. Mỗi command dùng frame LED mới, không xác nhận bằng frame cũ.
10. P88 thoát bằng bất kỳ phím và IO39 trở về 0.
