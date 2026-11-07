# Error Handling and Edge Cases

## Giải mã và hiển thị Totalizer

| Điều kiện | Xử lý hiện tại |
|---|---|
| Hàng 1 không có `P20` khi đọc tổng tiền | Bỏ frame, chờ frame mới; hết 3 giây thì retry |
| Hàng số tổng tiền có segment khác blank/0-9 | Bỏ toàn bộ frame, không ghép dữ liệu một phần |
| DP tổng tiền chỉ ở cột 3 | Ghép 6 chữ số với hệ số `1` |
| DP tổng tiền ở cột 1 và 4 | Ghép 6 chữ số với hệ số `10` |
| Tổng tiền không DP hoặc chỉ DP cột 1 | Ghép 6 chữ số với hệ số `10000` |
| DP tổng tiền có pattern khác | Bỏ frame và retry để tránh sai hệ số |
| Tổng lít thiếu DP bắt đầu hoặc thiếu chữ số thập phân | Bỏ frame và retry, không suy đoán giá trị |
| Tổng lít có DP thứ hai | Dùng DP thứ hai làm điểm kết thúc vùng thập phân |
| Snapshot có generation cũ | Bỏ qua để tránh lấy màn hình trước khi gửi phím |
| Một trong hai phần thất bại sau 3 lần | Đặt cả tiền và lít về 0; không hiển thị/publish dữ liệu cũ |
| Phần nguyên lít cần hơn 6 LED hoặc phần lẻ không còn chỗ cho DP | Không cắt chữ số, không bật Virtual LED và log lỗi |
| Người dùng yêu cầu thoát trong lúc đang đọc | Không bật IO39 sau khi đọc xong; kết thúc an toàn |
| Wi-Fi config/status đang giữ LED | Không bắt đầu Totalizer local; giữ quyền LED cho Wi-Fi |
| Nhấn phím khi đang xem Totalizer | Tiêu thụ phím, thoát, IO39 về 0; không chạy command khác |
| LED decode nhận màn hình chuẩn bị bơm `0 / 0.00` hoặc `0 / 0.000` | Tăng `prepare_generation`; Totalizer tự thoát, IO39 về 0 và trả LED cho mạch ngoài |
| Màn hình chuẩn bị bơm xuất hiện trong lúc đang đọc tổng | Sau khi đọc xong thấy generation đã đổi; không bật Virtual LED bằng dữ liệu Totalizer vừa đọc |
| Totalizer local hiển thị được nhưng time chưa sync | Không tạo `ts=0`; log cảnh báo, không gửi event và vẫn giữ màn hình |
| Totalizer local hiển thị được nhưng MQTT chưa ready/queue lỗi | Log cảnh báo, không retry blocking và vẫn giữ màn hình cho tới điều kiện thoát bình thường |
| Không đọc được `node_id` runtime từ NVS | Không tạo `event_id`; bỏ lần publish event nhưng không ảnh hưởng hiển thị |

## Totalizer Shortcut Slot 10

| Condition | Detection | Handling | State impact |
|---|---|---|---|
| Slot 10 is absent from NVS | Read returns `ESP_ERR_NOT_FOUND` | Use compatible default `P88` | No NVS write and no error ACK |
| Payload omits `physical_key_10` | Valid `shortcut_key_10` is the only field | Persist, read back, then ACK `ok` | Totalizer shortcut changes |
| Payload includes `physical_key_10` | Optional valid string is present | Persist and return it in ACK, but ignore it at runtime | It is never emitted as virtual keys |
| Shortcut is incomplete | Input is only a prefix of slot 10 | Wait for completion or keypad timeout | Totalizer does not start |
| Shortcut conflicts with another stored slot | Duplicate check fails | ACK `error/invalid_param` | Existing mapping remains unchanged |

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
| Raw calibration MQTT/boot/restore không khớp | Retry toàn bộ calibration tối đa 3 lần |
| Calibration shortcut local không khớp | Chỉ chạy 1 lần, gửi `C` để thoát và không retry |
| Calibration local thất bại | Không ghi mode mới; sau 500 ms mô phỏng snapshot cũ và hiển thị `E` tại hàng 3 cột 1 |
| Calibration local thành công | Sau 500 ms mô phỏng snapshot cũ và chỉ sáng DP tại hàng 3 cột 1 |
| Chưa có snapshot LED trước calibration local | Dùng nền trống, vẫn hiển thị DP hoặc `E` |
| Wi-Fi/Totalizer đang giữ LED | Không ghi đè luồng đang giữ LED; kết quả calibration không bị thay đổi |
| Shortcut Totalizer đọc thất bại | Không hiển thị dữ liệu cũ; thoát trạng thái active |
| Giá trị Totalizer vượt 6 LED mỗi hàng | Log lỗi và không cắt chữ số |

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
| `command_code` không có calibration mapping tương ứng | Log cảnh báo; giữ `command_value` rỗng thay vì suy đoán raw command từ mode khác |
| Queue giao dịch NVS schema version 2 | Migrate sang version 3, giữ dữ liệu cũ và exact-match mapping hiện tại để bổ sung `command_value` |
| Mapping thay đổi sau khi giao dịch đã bắt đầu | Không cập nhật transaction đang chạy hoặc pending; telemetry giữ `command_value` đã chụp lúc bắt đầu bơm |
| `last_price` chưa tồn tại sau nâng cấp firmware | Trạng thái lần đầu hợp lệ; dùng `unit_price=0` cho tới khi LED hoặc `set_unit_price` cung cấp giá |
| `last_price` bằng 0 hoặc lớn hơn 999999 | Xóa cache sai, tiếp tục chạy và chờ giá hợp lệ |
| Giá mới trùng giá NVS | Không commit Flash lại |
| `core_cfg` chưa tồn tại | Seed từ `device_config.h`, commit và readback |
| `core_cfg` sai magic/version/length | Khôi phục factory seed và log cảnh báo |
| Ghi/readback core config lỗi | Giữ cấu hình RAM cũ, trả lỗi storage/verification |
| OTA chỉ cập nhật app slot | Giữ nguyên toàn bộ NVS |
| `erase_flash` hoặc xóa NVS | Mất cấu hình; boot sau seed lại factory defaults |

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

## 7. Heartbeat

| Điều kiện | Xử lý |
|---|---|
| Task vừa khởi động, MQTT chưa ready | Retry mỗi 1 giây; gửi ngay khi MQTT ready, không chờ đủ 60 giây đầu |
| SNTP chưa sync | Vẫn publish với `ts=0` |
| RSSI chưa đọc được/ngoài miền hợp lệ | Vẫn publish với `RSSI=0` |
| Chưa có giá từ LED và NVS chưa có `last_price` | Vẫn publish với `data.unit_price=0` |
| MQTT enqueue thất bại | Log cảnh báo và retry sau 1 giây |
| Publish thành công | Chờ 60 giây trước heartbeat tiếp theo |

## 8. Time

| Điều kiện | Xử lý |
|---|---|
| Chưa SNTP sync | `time_manager_is_valid=false`, timestamp API trả lỗi/-1 |
| Mất Wi-Fi sau sync | System time tiếp tục chạy |
| Có IP lại | Yêu cầu sync lại |
| SNTP start lỗi | Log cảnh báo, không reset |

## 9. Bảo mật và production

Hiện trạng cần xử lý trước khi triển khai production:

- MQTT đang dùng TCP thường, không TLS.
- MQTT và Wi-Fi factory credential nằm trong header firmware.
- Secure Boot và Flash Encryption chưa bật.
- OTA dùng HTTPS và certificate bundle; GitHub repository private chưa được hỗ trợ.
- Bootloader rollback đã bật, nhưng Secure Boot/chữ ký ứng dụng chưa bật.
- Web Portal chỉ phù hợp mạng cấu hình cục bộ, chưa có CSRF/session authentication.

## 10. Log chẩn đoán theo module

| TAG | Nội dung cần xem |
|---|---|
| `MBI_SNIFFER` | SPI counters, COL, LED rows, offset/assembly |
| `PUMP_FILTER` | Start/stop lượt bơm, progressive data, validation, telemetry |
| `PUMP_STORE` | Pending NVS queue |
| `MQTT_MANAGER` | Connect, subscribe, publish queue, reconnect |
| `MQTT_COMMAND` | Param, retry, verification và ACK |
| `WIFI_MANAGER` | STA/AP, reconnect, credential trial |
| `TIME_MANAGER` | SNTP request và sync |
| `TELEMETRY_HEARTBEAT` | Payload đã queue hoặc lỗi queue/retry heartbeat |
| `KEY_OUTPUT` | Keypad, virtual key, shortcut Totalizer và IO39 |
| `OTA_MANAGER` | Manifest, version, download, self-test và rollback |

### OTA errors

| Điều kiện | Xử lý |
|---|---|
| OTA command sai envelope/param | ACK `error/invalid_param` |
| OTA đang chạy/chờ | ACK `rejected/busy` |
| GitHub private/404/TLS/network lỗi | Dừng OTA, giữ firmware hiện tại, log lỗi |
| Manifest quá 1024 byte hoặc JSON/version sai | Dừng OTA, không ghi flash |
| Remote version bằng/thấp hơn current | Không tải, không reboot |
| Image project/version không khớp manifest | Abort OTA, giữ boot partition hiện tại |
| Mất nguồn trong lúc tải | Boot app cũ; inactive slot chưa hoàn tất không được chọn |
| Firmware mới crash trước self-test | Bootloader rollback app cũ |
| Self-test đọc core NVS lỗi | Mark invalid và rollback |

## 11. Test regression tối thiểu

1. Boot khi màn hình đang có giao dịch cũ: không gửi telemetry.
2. `0/0` -> một mẫu kết quả -> đứng 10 giây: gửi đúng một giao dịch.
3. `0/0` -> quay lại đúng giao dịch cũ: không gửi.
4. Bơm tăng qua nhiều bước: lấy mẫu cuối cùng.
5. Mất Wi-Fi/time khi hoàn tất: lưu NVS, reboot, gửi lại khi ready.
6. Gửi command trùng `request_id`: lần hai bị reject.
7. Mất router và có lại: Wi-Fi, SNTP, MQTT tự phục hồi.
8. Sai password portal: credential cũ không bị mất.
9. Mỗi command dùng frame LED mới, không xác nhận bằng frame cũ.
10. Totalizer local thoát bằng bất kỳ phím và IO39 trở về 0.
11. OTA cùng version: ACK accepted, không tải/không reboot.
12. OTA version mới: tải, reboot, self-test và mark-valid.
13. Ngắt nguồn trước mark-valid: bootloader rollback firmware cũ.
14. Nâng cấp từ firmware chưa có `last_price`: boot bình thường, heartbeat gửi giá 0 cho tới khi có giá hợp lệ.
15. Sau khi lưu giá hợp lệ, reboot khi màn hình chưa đổi: heartbeat dùng lại giá từ NVS.
16. Heartbeat khi chưa sync time hoặc chưa đọc được RSSI: vẫn gửi với giá trị fallback 0.
17. `set_mqtt_config` sai credential: rollback về broker cũ và không lặp reboot vô hạn.
