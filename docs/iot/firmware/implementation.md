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
| `tbmq/keyboard/<node_id>/noti` | Node | Server | 1 | false |

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

Ví dụ ACK lỗi param; các ACK lỗi command cụ thể giữ nguyên envelope này và chỉ
thay `ack_to`, `description` theo bảng/luồng tương ứng:

```json
{
  "ts": 1794000002,
  "version": "1.3",
  "request_id": "cmd-unit-price-invalid-001",
  "ack_to": "set_unit_price",
  "result": "error",
  "description": "invalid_param",
  "reported": {},
  "time_device": "07/11/2026 10:00:02"
}
```

Request trùng `request_id` đã được xử lý trả `result=rejected`,
`description=duplicate_request` và `reported: {}`.

## 6. Command đã hỗ trợ

Tất cả ví dụ trong mục này dùng hai topic sau, với `node_kbd_029` chỉ là Node
mẫu:

```text
Server -> Node: tbmq/keyboard/node_kbd_029/command
Node -> Server: tbmq/keyboard/node_kbd_029/ack
```

`request_id` trong ACK phải giữ nguyên tuyệt đối từ command và `ack_to` phải
bằng đúng `cmd`. Các giá trị `ts` và `time_device` trong ACK dưới đây chỉ là ví
dụ; firmware lấy cả hai từ cùng một snapshot của `time_manager`.

### 6.1 `set_hash_key_lock`

Command server gửi:

```json
{
  "ts": 1794000000,
  "version": "1.3",
  "request_id": "cmd-hash-lock-001",
  "cmd": "set_hash_key_lock",
  "param": {
    "locked": true
  }
}
```

ACK thành công:

```json
{
  "ts": 1794000001,
  "version": "1.3",
  "request_id": "cmd-hash-lock-001",
  "ack_to": "set_hash_key_lock",
  "result": "ok",
  "description": "hash_key_lock_updated",
  "reported": {
    "hash_key_locked": true
  },
  "time_device": "07/11/2026 10:00:01"
}
```

- Lưu NVS `device_cfg/hash_lock`.
- `true`: bỏ qua phím `#` vật lý; phím ảo vẫn hoạt động.
- ACK thành công: `hash_key_lock_updated`, reported `hash_key_locked`.
- ACK lỗi: `error/invalid_param`, `error/storage_error` hoặc
  `error/verification_failed`.

### 6.2 `set_price_edit_lock`

Command server gửi:

```json
{
  "ts": 1794000010,
  "version": "1.3",
  "request_id": "cmd-price-lock-001",
  "cmd": "set_price_edit_lock",
  "param": {
    "locked": true
  }
}
```

ACK thành công:

```json
{
  "ts": 1794000011,
  "version": "1.3",
  "request_id": "cmd-price-lock-001",
  "ack_to": "set_price_edit_lock",
  "result": "ok",
  "description": "price_edit_lock_updated",
  "reported": {
    "price_edit_locked": true
  },
  "time_device": "07/11/2026 10:00:11"
}
```

- Lưu NVS `device_cfg/price_lock` và readback trước khi ACK `ok`.
- Chưa nối vào logic khóa thao tác đổi giá.
- Giữ trạng thái qua reboot và OTA app.
- ACK thành công: `price_edit_lock_updated`, reported `price_edit_locked`.
- ACK lỗi: `error/invalid_param`, `error/storage_error` hoặc
  `error/verification_failed`.

### 6.3 `set_unit_price`

Command server gửi:

```json
{
  "ts": 1794000020,
  "version": "1.3",
  "request_id": "cmd-unit-price-001",
  "cmd": "set_unit_price",
  "param": {
    "unit_price": 31000
  }
}
```

ACK thành công sau khi LED readback đúng:

```json
{
  "ts": 1794000028,
  "version": "1.3",
  "request_id": "cmd-unit-price-001",
  "ack_to": "set_unit_price",
  "result": "ok",
  "description": "unit_price_updated",
  "reported": {
    "unit_price": 31000
  },
  "time_device": "07/11/2026 10:00:28"
}
```

- Khoảng hợp lệ: 1..999999, integer.
- Chuỗi phím: function key hiệu lực slot 3 (mặc định `C#44504EP`) + giá + `E`.
- Xác nhận LED: hàng 1 là `E0`, hàng 3 đúng giá.
- Retry toàn bộ tối đa 3 lần; mỗi lần retry bắt đầu bằng `C`.
- Kết thúc thành công hoặc thất bại đều gửi `CC` để thoát.
- Chỉ ACK sau khi verify và gửi `CC`: thành công dùng `result=ok`,
  `description=unit_price_updated`; thất bại dùng `result=error`,
  `description=verification_failed`.
- Mỗi phím giữ `AUTO_KEY_HOLD_TIME_MS=60 ms`; khoảng nghỉ giữa hai phím
  của command là `VIRTUAL_KEY_INTERVAL_MS=60 ms`.
- ACK lỗi param dùng `error/invalid_param`; verify LED thất bại dùng
  `error/verification_failed` và `reported` rỗng.

### 6.4 `get_totalizer`

`param` phải rỗng.

Command server gửi:

```json
{
  "ts": 1794000030,
  "version": "1.3",
  "request_id": "cmd-get-totalizer-001",
  "cmd": "get_totalizer",
  "param": {}
}
```

ACK thành công:

```json
{
  "ts": 1794000040,
  "version": "1.3",
  "request_id": "cmd-get-totalizer-001",
  "ack_to": "get_totalizer",
  "result": "ok",
  "description": "totalizer_read_success",
  "reported": {
    "total_amount_vnd": 10862100,
    "total_volume_l": 273.6
  },
  "time_device": "07/11/2026 10:00:40"
}
```

Nếu không xác nhận được một trong hai tổng sau các lần retry, ACK là
`error/verification_failed` với `reported: {}`. Param không rỗng trả
`error/invalid_param`.

Luồng tổng tiền:

```text
function key slot 1, tách E cuối -> E -> chờ P20 -> decode hàng 2 -> C
```

Luồng tổng lít:

```text
function key slot 2 -> chờ 1 giây -> decode màn hình -> C
```

Mỗi phần retry tối đa 3 lần. Cả hai thành công mới:

- ACK `totalizer_read_success` với hai giá trị trong `reported`;
- publish thêm totalizer telemetry.

#### Cách đọc tổng tiền

Implementation dùng `read_total_amount()` và chỉ chấp nhận snapshot LED mới:

1. Đọc function key slot 1 (mặc định `C#7733E`) và gửi phần trước `E` cuối.
2. Chờ một khoảng phím, lưu `baseline_generation` của LED decoder.
3. Gửi `E`.
4. Poll snapshot mỗi 50 ms, tối đa 3000 ms.
5. Bỏ qua snapshot có `generation` bằng baseline để không dùng dữ liệu cũ.
6. Kiểm tra hàng 1: cột 1-3 trống, cột 4-6 đúng `P20`.
7. Giải mã hàng 2 từ trái sang phải. Ô trống được bỏ qua; các ô còn lại phải
   là số `0-9`.
8. Lấy DP mask của cả 6 cột để xác định hệ số phần giá trị bị ẩn khỏi màn hình.
9. Nhân dãy số đang thấy với hệ số, lưu vào `uint64_t total_amount_vnd`, đồng
   thời giữ nguyên 6 byte segment hàng 2 để hiển thị local, rồi gửi `C`.

Bảng hệ số tổng tiền theo đúng format màn hình hiện tại:

| DP trên hàng 2 | Ví dụ 6 chữ số | Hệ số | `total_amount_vnd` |
|---|---:|---:|---:|
| Chỉ cột 3 | `110.450` | `1` | `110450` |
| Cột 1 và cột 4 | `1.104.50` | `10` | `1104500` |
| Cột 2 và cột 5 | `10.862.1` | `100` | `10862100` |
| Không có DP | `110450` | `10000` | `1104500000` |
| Chỉ cột 1 | `1.10450` | `10000` | `1104500000` |

Mẫu DP khác bốn trường hợp trên bị từ chối để tránh gửi sai tổng tiền.

Ví dụ:

```text
LED 01-06: [  ] [  ] [  ] [P ] [2 ] [0 ]
LED 07-12: [1 ] [1 ] [0.] [4 ] [5 ] [0 ]
```

Hàng 2 có DP tại cột 3 nên hệ số là `1`; kết quả
`total_amount_vnd = 110450`.

Ví dụ dưới 10 triệu:

```text
LED 07-12: [1.] [1 ] [0 ] [4.] [5 ] [0 ]
```

DP ở cột 1 và 4 nên `110450 * 10 = 1104500`.

Ví dụ giá trị lớn dùng hệ số 10000:

```text
LED 07-12: [1 ] [1 ] [0 ] [4 ] [5 ] [0 ]
LED 07-12: [1.] [1 ] [0 ] [4 ] [5 ] [0 ]
```

Cả mẫu không DP và mẫu chỉ có DP cột 1 đều được giải mã thành
`110450 * 10000 = 1104500000`.

#### Cách đọc tổng lít

Implementation dùng `read_total_volume()`:

1. Lưu generation hiện tại rồi gửi function key slot 2 (mặc định `C#44504ET`).
2. Chờ `TOTALIZER_DISPLAY_SETTLE_MS=1000 ms` để màn hình ổn định.
3. Poll snapshot mới mỗi 50 ms, tối đa 3000 ms.
4. Giải mã toàn bộ chữ số hợp lệ ở hàng 1 thành phần nguyên.
5. Ở hàng 2, DP đầu tiên là mốc bắt đầu phần thập phân. Các chữ số từ mốc
   này được ghép theo thứ tự; nếu gặp DP thứ hai thì dừng.
6. Tính `total_volume_l = integer_part + fractional_part/fractional_scale`.
7. Gửi `C` để thoát màn hình tổng lít.

Ví dụ:

```text
LED 01-06: [  ] [  ] [  ] [  ] [  ] [2 ]
LED 07-12: [ .] [0 ] [  ] [  ] [ .] [7 ]
```

Hàng 1 cho phần nguyên `2`. DP đầu tiên mở vùng thập phân, chữ số `0` được
lấy và DP thứ hai kết thúc vùng này. Kết quả là `total_volume_l = 2.0`.

Nếu thiếu DP bắt đầu, không có chữ số thập phân, xuất hiện segment lạ, hoặc
không có frame mới thì lần đọc thất bại. Mỗi phần retry tối đa 3 lần. Nếu một
phần vẫn thất bại, `read_totalizer()` đặt cả hai output về `0` và không dùng
dữ liệu cũ.

#### Hiển thị Totalizer bằng phím tắt local

Phím tắt ở slot 10 gọi `read_totalizer_result()` rồi hiển thị một trang 3x6:

- hàng 1: copy nguyên 6 segment tổng tiền đã đọc, gồm cả vị trí DP;
- hàng 2: phần thập phân của tổng lít nằm bên phải; một ô DP-only đặt ngay
  trước các chữ số thập phân;
- hàng 3: phần nguyên của tổng lít, canh phải trong 6 cột;
- phần lít giữ tối đa 3 số thập phân, bỏ số `0` dư ở cuối nhưng luôn giữ ít
  nhất một số sau dấu chấm.

Nếu màn hình nguồn của tổng tiền là `[1.][1][0][4][5][0]`, hàng 1 của màn
hình Totalizer giữ nguyên đúng pattern đó, không dựng lại từ số
`total_amount_vnd`:

```text
LED 01-06: [1.] [1 ] [0 ] [4 ] [5 ] [0 ]
```

Nếu tổng lít là `266.5`, phần lẻ `.5` nằm ở góc phải hàng 2 và phần nguyên
`266` nằm ở hàng 3:

```text
LED 07-12: [  ] [  ] [  ] [  ] [ .] [5 ]
LED 13-18: [  ] [  ] [  ] [2 ] [6 ] [6 ]
```

Hàng tiền luôn có đúng 6 segment lấy từ decoder. Phần nguyên tổng lít tối đa
6 chữ số; phần thập phân tối đa 5 chữ số do cần dành một cột cho DP-only.
Firmware không cắt mất chữ số: nếu phần lít không vừa thì log lỗi và không bật
màn hình Totalizer.

Khi format hợp lệ, task gọi `control_display_led_begin_virtual()`, kéo IO39
lên `1` để ESP32 chiếm quyền LED và quét cả ba hàng. Chế độ xem kết thúc khi
người dùng nhấn bất kỳ phím vật lý nào hoặc bộ lọc giao dịch nhận được màn
hình chuẩn bị bơm với hàng tiền bằng `0` và hàng lít bằng `0.00`/`0.000`.
Phím thoát được tiêu thụ, không kích hoạt shortcut khác.

`pump_transaction_filter` tăng `prepare_generation` tại đúng nhánh bắt đầu
lượt bơm. Task Totalizer lưu generation lúc được kích hoạt và kiểm tra lại cả
sau khi đọc tổng lẫn mỗi `TOTALIZER_VIEW_POLL_MS=50 ms` trong thời gian hiển
thị. Nếu generation thay đổi trong lúc đang đọc, firmware không bật màn hình
Totalizer. Nếu thay đổi khi đang hiển thị, firmware thoát và trả LED ngay.

Khi thoát, IO39 về `0` và
`control_display_led_end_virtual()` khôi phục buffer màn hình ngoài. Nếu trạng
thái Wi-Fi đang cần LED thì quyền hiển thị được bàn giao cho luồng Wi-Fi.

Sau khi IO39 được bật và trang Totalizer local bắt đầu hiển thị thành công,
firmware chủ động publish một event chốt ca lên
`tbmq/keyboard/<node_id>/event`, QoS 1, retain false. Event này chỉ áp dụng
cho phím tắt Totalizer vật lý; command MQTT `get_totalizer` và `close_shift`
giữ nguyên luồng riêng.

```json
{
  "ts": 1793933258,
  "version": "1.8",
  "event_id": "evt-node_kbd_029-1793933258",
  "event": "close_shif",
  "source": "device",
  "trigger": "physical_key",
  "result": "ok",
  "reported": {
    "total_amount_vnd": 8665400,
    "total_volume_l": 273.6
  },
  "time_device": "06/11/2026 09:47:38"
}
```

`event_id` được tạo từ `node_id` đang dùng trong NVS và cùng `ts` của
`time_manager_get_snapshot()`: `evt-<node_id>-<ts>`. Hai giá trị trong
`reported` lấy trực tiếp từ cùng `totalizer_result_t` đang được hiển thị.
Giá trị wire-format của field `event` hiện là `close_shif` theo contract server
đã cung cấp.

Publish event chỉ queue MQTT và không thay đổi `s_totalizer_view_active` hay
`s_totalizer_view_exit_requested`. Nếu time chưa valid hoặc MQTT chưa ready,
firmware log cảnh báo và vẫn giữ màn hình Totalizer; event không được dựng với
timestamp giả và không làm tắt LED.

### 6.5 `close_shift`

`param` phải rỗng. Reuse `read_totalizer()` giống `get_totalizer` nhưng chỉ ACK:

Command server gửi:

```json
{
  "ts": 1794000100,
  "version": "1.3",
  "request_id": "cmd-close-shift-001",
  "cmd": "close_shift",
  "param": {}
}
```

ACK thành công:

```json
{
  "ts": 1794000110,
  "version": "1.3",
  "request_id": "cmd-close-shift-001",
  "ack_to": "close_shift",
  "result": "ok",
  "description": "shift_closed",
  "reported": {
    "total_amount_vnd": 10862100,
    "total_volume_l": 273.6
  },
  "time_device": "07/11/2026 10:01:50"
}
```

- description `shift_closed`;
- reported có `total_amount_vnd`, `total_volume_l`.
- Đọc tổng thất bại trả `error/verification_failed`; param không rỗng trả
  `error/invalid_param`.

Backend chịu trách nhiệm bảo đảm chốt ca đúng một lần.

### 6.6 `reset_totalizer`

Command server gửi:

```json
{
  "ts": 1794000120,
  "version": "1.3",
  "request_id": "cmd-reset-totalizer-001",
  "cmd": "reset_totalizer",
  "param": {
    "confirm": true
  }
}
```

ACK thành công sau khi cả hai tổng đã được readback bằng 0:

```json
{
  "ts": 1794000140,
  "version": "1.3",
  "request_id": "cmd-reset-totalizer-001",
  "ack_to": "reset_totalizer",
  "result": "ok",
  "description": "totalizer_reset",
  "reported": {
    "total_amount_vnd": 0,
    "total_volume_l": 0
  },
  "time_device": "07/11/2026 10:02:20"
}
```

Tổng tiền:

```text
function key slot 4, tách hậu tố E000C -> E -> verify P20
-> 000 -> verify E0 -> C
```

Tổng lít:

```text
C#845443E -> verify P08 -> 0 -> verify hàng 2 = 0
-> E -> verify E0 -> C
```

Mỗi phần retry 3 lần. ACK `totalizer_reset` chỉ khi cả hai thành công, reported bằng 0.
Thiếu `confirm:true`, có field param thừa hoặc sai kiểu trả
`error/invalid_param`; readback thất bại trả `error/verification_failed`.

### 6.7 `set_shortcut_mapping`

Slots 1-9 keep the normal `shortcut_key_N -> physical_key_N` behavior. Both
fields are required and the matched physical shortcut emits the configured
virtual-key sequence.

Slot 10 is reserved for the local Totalizer view shortcut. Only
`shortcut_key_10` is required:

```json
{
  "ts": 1794000145,
  "version": "1.3",
  "request_id": "cmd-totalizer-shortcut-001",
  "cmd": "set_shortcut_mapping",
  "param": {
    "shortcut_key_10": "P9E"
  }
}
```

ACK slot 10 vẫn dùng cùng contract với slot 1-9. Khi server không gửi
`physical_key_10`, readback hiện trả giá trị tương thích mặc định `P88`:

```json
{
  "ts": 1794000146,
  "version": "1.3",
  "request_id": "cmd-totalizer-shortcut-001",
  "ack_to": "set_shortcut_mapping",
  "result": "ok",
  "description": "shortcut_mapping_updated",
  "reported": {
    "mapping": {
      "physical_key": "P88",
      "shortcut_key": "P9E"
    },
    "revision": 4
  },
  "time_device": "07/11/2026 10:02:26"
}
```

`physical_key_10` is optional for backward-compatible server payloads. Its
value is persisted and returned in ACK readback, but is ignored at runtime and
is never emitted as virtual keys. After NVS readback succeeds, entering `P9E` on the physical
keypad starts the common `read_totalizer()` flow used by MQTT `get_totalizer`.
It does not emit a virtual-key mapping and does not send an MQTT ACK. If slot
10 has never been configured, the compatible default is `P88`. The configured
value persists across reboot and OTA through the existing shortcut NVS blob.
The successful ACK uses the same schema as slots 1-9 and reports both
`mapping.shortcut_key`, `mapping.physical_key`, and the updated `revision`.

Mỗi command cập nhật đúng một slot từ 1 đến 10:

Command server gửi cho slot thường 1-9:

```json
{
  "ts": 1794000150,
  "version": "1.3",
  "request_id": "cmd-shortcut-001",
  "cmd": "set_shortcut_mapping",
  "param": {
    "physical_key_1": "C#7733E",
    "shortcut_key_1": "P123"
  }
}
```

ACK thành công sau khi ghi và readback NVS:

```json
{
  "ts": 1794000151,
  "version": "1.3",
  "request_id": "cmd-shortcut-001",
  "ack_to": "set_shortcut_mapping",
  "result": "ok",
  "description": "shortcut_mapping_updated",
  "reported": {
    "mapping": {
      "physical_key": "C#7733E",
      "shortcut_key": "P123"
    },
    "revision": 5
  },
  "time_device": "07/11/2026 10:02:31"
}
```

- Hai field phải cùng suffix slot.
- Ký tự cho phép: `0-9`, `#`, `$`, `C`, `E`, `L`, `P`, `T`, `V`.
- `shortcut_key`: tối đa 16 ký tự.
- `physical_key`: tối đa 32 ký tự.
- Không cho trùng shortcut ở slot khác.
- Ghi NVS, readback và kiểm tra revision trước ACK.
- ACK `shortcut_mapping_updated`, reported chứa `mapping` và `revision`.
- ACK lỗi có thể là `invalid_param`, `mapping_limit_reached`,
  `reported_mismatch`, `storage_error` hoặc `internal_error`; `result` là
  `error` và `reported` rỗng.

Khi người dùng nhập shortcut vật lý, sequence output được đưa qua queue. Phím ảo không đi lại qua bộ nhận dạng shortcut nên không tạo vòng lặp.

### 6.7.1 `set_function_key_config`

Command này cấu hình chuỗi phím hệ thống, độc lập với bảng shortcut vật lý ở
mục 6.7. Bảng có 10 slot; 5 slot đầu đã được gắn vào runtime:

| Slot | Chức năng | Factory key |
|---:|---|---|
| 1 | Đọc tổng tiền | `C#7733E` |
| 2 | Xem tổng lít | `C#44504ET` |
| 3 | Tiền tố đổi đơn giá | `C#44504EP` |
| 4 | Xóa tổng tiền | `C#7733E000C` |
| 5 | Vào calibration | `C#122973E` |
| 6-10 | Dự phòng | Chưa gắn chức năng runtime |

Ví dụ đổi chuỗi đọc tổng tiền ở slot 1:

```json
{
  "ts": 1794000150,
  "version": "1.3",
  "request_id": "cmd-function-key-config-001",
  "cmd": "set_function_key_config",
  "param": {
    "factory_key_1": "C#7733E",
    "edited_key_1": "C#8833E"
  }
}
```

ACK chỉ được gửi `ok` sau khi ghi NVS và readback đúng:

```json
{
  "ts": 1794000151,
  "version": "1.3",
  "request_id": "cmd-function-key-config-001",
  "ack_to": "set_function_key_config",
  "result": "ok",
  "description": "function_key_updated",
  "reported": {
    "mapping": {
      "factory_key_1": "C#7733E",
      "edited_key_1": "C#8833E"
    },
    "revision": 1
  },
  "time_device": "07/11/2026 10:02:31"
}
```

- Hai field phải cùng suffix từ 1 đến 10 và `param` không được có field thừa.
- Slot 1-5 yêu cầu `factory_key_N` đúng factory key đã biên dịch; field này xác
  định slot chứ không thay đổi factory key.
- Khi chưa có cấu hình NVS, `edited_key` của slot 1-5 được seed bằng factory
  key nên hành vi cũ được giữ nguyên.
- Mỗi thay đổi thực sự tăng `revision`; gửi lại cùng giá trị không tăng revision.
- Slot 1 giữ ký tự `E` cuối để firmware chụp generation trước khi bấm `E` và
  verify `P20`.
- Slot 4 bắt buộc kết thúc bằng `E000C`. Firmware vẫn thực hiện theo từng bước:
  phần tiền tố -> `E` -> verify `P20` -> `000` -> verify `E0` -> `C`.
- Slot 6-10 có thể được lưu trước cho mở rộng, nhưng hiện chưa được gọi bởi
  chức năng runtime.
- Param sai trả `error/invalid_param`; ghi NVS lỗi trả `error/storage_error`;
  readback sai trả `error/verification_failed`.

### 6.8 `send_calibration_command`

Mỗi command cập nhật một slot từ 1 đến 5:

Command server gửi:

```json
{
  "ts": 1794000160,
  "version": "1.3",
  "request_id": "cmd-calibration-001",
  "cmd": "send_calibration_command",
  "param": {
    "name_3": "P555",
    "raw_command_3": "2530"
  }
}
```

ACK thành công chỉ sau khi calibration và NVS readback đều đúng:

```json
{
  "ts": 1794000170,
  "version": "1.3",
  "request_id": "cmd-calibration-001",
  "ack_to": "send_calibration_command",
  "result": "ok",
  "description": "calibration_command_applied",
  "reported": {
    "name": "P555",
    "command_echo": "2530",
    "mode_calibration": "P555"
  },
  "time_device": "07/11/2026 10:02:50"
}
```

- `name`: shortcut hợp lệ, tối đa 16 ký tự.
- `raw_command`: chỉ số, dài 1..6.
- Gửi function key hiệu lực của slot 5 (mặc định `C#122973E`), verify `P06`.
- Chờ `CALIBRATION_RAW_COMMAND_DELAY_MS=50 ms` rồi nhập raw command.
- Verify hàng 2 đúng raw command.
- Gửi `E`, verify hàng 1 `E0` và hàng 2 vẫn đúng.
- Gửi `C` để thoát.
- Command MQTT, boot reapply và mode restore retry tối đa 3 lần.
- Calibration shortcut local chỉ thực hiện đúng 1 lần, không retry.
- Chỉ sau verify thành công mới lưu mapping và `mode_calibration=name`.
- ACK `calibration_command_applied`, reported gồm `name`, `command_echo`, `mode_calibration`.
- Param sai trả `error/invalid_param`; thao tác LED/readback sai trả
  `error/verification_failed`; ghi NVS lỗi trả `error/storage_error`.

Khi boot, firmware không ghi calibration lại ngay. Nó phát function key hiệu
lực của slot 5 (mặc định `C#122973E`), chờ
frame mới có `P06`, đọc hàng 2 và so sánh với `raw_command` của
`mode_calibration` trong NVS. DP định dạng trên hàng 2 được bỏ qua khi so sánh.
Nếu hai giá trị bằng nhau, firmware gửi `C` để thoát. Nếu khác, firmware ghi
nhận noti pending `calibration_mismatch`, áp dụng lại raw command và verify
`E0` như flow calibration bình thường.

Sau khi thoát, hai frame hàng 3 liên tiếp phải decode ra cùng một giá trước khi
so sánh với `last_unit_price` trong NVS. Giá khác nhau tạo noti pending
`price_mismatch`. Hai thông báo được publish lên topic `noti` sau khi MQTT
ready và SNTP hợp lệ;
không có giá NVS hoặc không decode được hàng 3 thì chỉ log, không báo mismatch.

Hai thông báo boot là dữ liệu Node chủ động publish, không phải ACK command và
server không gửi command để kích hoạt trực tiếp. Ví dụ calibration mismatch:

```json
{
  "ts": 1794000180,
  "version": "1.8",
  "event_id": "evt-node_kbd_029-1794000180-000001",
  "event": "calibration_mismatch",
  "source": "device",
  "trigger": "system",
  "severity": "error",
  "status": "active",
  "data": {
    "parameter": "mode_calibration",
    "expected_command_code": "P555",
    "expected_command_value": "2530",
    "actual_command_value": "2500"
  },
  "time_device": "07/11/2026 10:03:00"
}
```

Ví dụ giá đọc từ LED khác giá lưu trong NVS:

```json
{
  "ts": 1794000181,
  "version": "1.8",
  "event_id": "evt-node_kbd_029-1794000181-000002",
  "event": "price_mismatch",
  "source": "device",
  "trigger": "system",
  "severity": "warning",
  "status": "active",
  "data": {
    "expected_price_vnd": 25000,
    "actual_price_vnd": 24500
  },
  "time_device": "07/11/2026 10:03:01"
}
```

Slot 1 là mode calibration tạm thời. Nếu thiết bị đang ở mode thuộc slot 2-5
rồi nhận và áp dụng thành công `name_1/raw_command_1`, firmware lưu mode cũ
trong NVS `device_cfg/cal_restore`. Sau khi một giao dịch mang command code của
mode 1 hoàn tất, timer một lần chờ 30 giây rồi chạy lại calibration của mode cũ
qua cùng state machine. Khi verify thành công, `mode_calibration` được trả về
mode cũ và trạng thái tạm được xóa. Shortcut calibration local vào slot 1 dùng
cùng cơ chế lưu mode cũ và hoàn nguyên này. Command hoặc shortcut local chuyển
sang mode 2-5 sẽ hủy lịch hoàn nguyên đang chờ. Nếu một lượt hoàn nguyên thất
bại sau 3 lần verify nội bộ, firmware tiếp tục lên lịch thử lại sau 30 giây và
lặp cho tới khi về được mode cũ hoặc phát hiện thiết bị đã chuyển sang mode
khác.

Calibration shortcut local reuse cùng state machine và mutex với MQTT nhưng đặt
`max_attempts=1`; nó không phát MQTT ACK. Trước khi phát phím, task chụp đủ
snapshot LED 3 hàng x 6 cột. Sau khi calibration kết thúc, firmware chờ 500 ms,
dùng snapshot này làm nền rồi ghi đè riêng hàng 3 cột 1:

- thành công: chỉ sáng chân DP (`0x80`);
- thất bại: hiển thị chữ `E` (`0x79`).

Firmware bật IO39 và giữ màn hình kết quả trong 1 giây, sau đó kéo IO39 về 0 và
trả buffer LED về dữ liệu màn hình thật. Nếu chưa từng có snapshot hợp lệ thì
dùng nền trống nhưng vẫn hiển thị DP/E. Nếu Wi-Fi đang chiếm quyền LED thì không
ghi đè; nếu trạng thái Wi-Fi xuất hiện trong thời gian hiển thị thì quyền LED
được bàn giao cho luồng Wi-Fi.

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

ACK cho biết request đã được đưa vào OTA worker, chưa có nghĩa firmware mới đã
được tải và xác nhận thành công:

```json
{
  "ts": 1794000201,
  "version": "1.3",
  "request_id": "cmd-ota-001",
  "ack_to": "OTA",
  "result": "ok",
  "description": "ota_accepted",
  "reported": {},
  "time_device": "07/11/2026 10:03:21"
}
```

- Command name phân biệt hoa/thường và phải đúng `OTA`.
- Handler đưa request vào queue rồi ACK `ok/ota_accepted`; không tải firmware trong MQTT callback/task.
- Nếu đã có OTA chạy hoặc chờ: ACK `rejected/busy`.
- Payload sai hoặc `param` không rỗng: ACK `error/invalid_param`.
- OTA worker đang bận: ACK `rejected/busy` với `reported: {}`.
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

### 6.10 `set_mqtt_config`

Command server gửi:

```json
{
  "ts": 1791007000,
  "version": "1.3",
  "request_id": "cmd-mqtt-config-001",
  "cmd": "set_mqtt_config",
  "param": {
    "mqtt_broker_host": "161.248.146.170",
    "mqtt_broker_port": 1883,
    "mqtt_username": "node_user",
    "mqtt_password": "new_password"
  }
}
```

ACK khi cấu hình khác hiện tại và đã được stage vào NVS:

```json
{
  "ts": 1791007001,
  "version": "1.3",
  "request_id": "cmd-mqtt-config-001",
  "ack_to": "set_mqtt_config",
  "result": "ok",
  "description": "mqtt_config_staged",
  "reported": {
    "mqtt_broker_host": "161.248.146.170",
    "mqtt_broker_port": 1883,
    "mqtt_username": "node_user",
    "password_configured": true
  },
  "time_device": "30/09/2026 18:36:41"
}
```

Nếu bốn giá trị giống cấu hình đang chạy, ACK có cùng `reported` nhưng
`description=mqtt_config_unchanged` và thiết bị không reboot.

- Host không chứa scheme như `mqtt://`; port phải trong `1..65535`.
- Password không được ghi ra log hoặc trả lại trong ACK.
- Nếu cấu hình không đổi: ACK `ok/mqtt_config_unchanged`, không reboot.
- Nếu có thay đổi: lưu candidate và cấu hình cũ vào
  `device_cfg/mqtt_trial`, ACK `ok/mqtt_config_staged`, chờ 2 giây rồi reboot.
- Sau reboot, MQTT dùng candidate. Khi connect và subscribe command thành công,
  candidate được xác nhận và bản trial bị xóa.
- Nếu broker không ready trong 60 giây hoặc từ chối xác thực, firmware phục hồi
  cấu hình cũ và reboot.
- ACK `reported` chỉ chứa host, port, username và `password_configured=true`.
- Param sai trả `error/invalid_param`; đang có trial khác trả `error/busy`;
  lỗi NVS trả `error/storage_error`; không đọc được core config trả
  `error/internal_error`.

## 7. Command khai báo nhưng chưa triển khai

Các tên sau xuất hiện trong danh sách command nhưng chưa có handler:

- `set_reset_shortcut`
- `delete_shortcut_mapping`
- `get_reported_state`

Chúng hiện trả `result=rejected`, `description=unsupported_command`.

Ví dụ server gửi một command chưa hỗ trợ:

```json
{
  "ts": 1794000300,
  "version": "1.3",
  "request_id": "cmd-unsupported-001",
  "cmd": "get_reported_state",
  "param": {}
}
```

ACK:

```json
{
  "ts": 1794000301,
  "version": "1.3",
  "request_id": "cmd-unsupported-001",
  "ack_to": "get_reported_state",
  "result": "rejected",
  "description": "unsupported_command",
  "reported": {},
  "time_device": "07/11/2026 10:05:01"
}
```

## 8. Transaction telemetry

Topic: `tbmq/keyboard/<node_id>/telemetry`.

Đây là dữ liệu Node chủ động gửi khi giao dịch bơm hoàn tất, không phải command
từ server và không có ACK MQTT tương ứng. Payload publish mẫu nằm bên dưới.

### Giải mã `amount_vnd` từ hàng 1

Hàng 1 của màn hình giao dịch dùng cùng quy tắc DP với hàng 2 khi đọc tổng
tiền. Firmware ghép các chữ số hợp lệ từ trái sang phải, sau đó áp dụng hệ số
theo DP mask:

| Bố cục hàng 1 | Hệ số `amount_vnd` |
|---|---:|
| Chỉ DP cột 3 | `1` |
| DP cột 1 và cột 4 | `10` |
| Không DP, cột 1-3 trống và có tối đa 3 số bên phải | `1` |
| Không DP và đủ 6 cột đều là số | `10000` |
| Chỉ DP cột 1 và đủ 6 cột đều là số | `100000` |

Ví dụ `[ ][ ][7.][8][5][0]` được giải mã thành `7850 VNĐ`, còn
`[ ][ ][ ][6][9][0]` được giải mã thành `690 VNĐ`. Mẫu bố cục/DP khác các
trường hợp trên bị từ chối. `amount_vnd` dùng `uint64_t`; quy tắc này chỉ thay
đổi hàng 1, còn hàng 2 vẫn được ghép thành `volume_ml` và hàng 3 thành
`unit_price` theo cơ chế giao dịch hiện tại.

Field `version` của mọi telemetry lấy từ app descriptor qua
`firmware_version_get()`. Đây cũng là version OTA được sinh từ `PROJECT_VER`;
không hard-code `1.3` trong telemetry. Version `1.3` chỉ là schema của
command/ACK. `PROJECT_VER` hiện tại là `1.8`.

```json
{
  "ts": 1789369621,
  "version": "1.8",
  "data": {
    "unit_price": 25000,
    "amount_vnd": 7850,
    "volume_ml": 314,
    "command_code": "P555",
    "command_value": "2530",
    "time_device": "14/09/2026 14:07:01",
    "is_buffered": false,
    "RSSI": -30
  }
}
```

| Field | Kiểu | Đơn vị / quy tắc |
|---|---|---|
| `ts` | integer 64-bit | Unix UTC giây |
| `version` | string | firmware `PROJECT_VER`, cùng giá trị với OTA app version |
| `unit_price` | number | VNĐ/L |
| `amount_vnd` | number | VNĐ |
| `volume_ml` | number | mL |
| `command_code` | string | mode calibration tại lúc bắt đầu lượt bơm |
| `command_value` | string | `raw_command` được ánh xạ chính xác với `command_code` tại lúc bắt đầu lượt bơm |
| `time_device` | string | `DD/MM/YYYY HH:mm:ss`, Asia/Ho_Chi_Minh |
| `is_buffered` | boolean | `true` khi đọc từ NVS pending |
| `RSSI` | integer | dBm |

Validation trước publish:

- ba giá trị giao dịch phải lớn hơn 0;
- `command_code` hợp lệ; firmware tra exact-match trong calibration mapping để
  đóng băng `command_value` tương ứng;
- chênh lệch tiền so với `unit_price * volume_ml / 1000` không quá 200 VNĐ;
- time valid, lấy được RSSI và MQTT ready.

Nếu chưa đủ điều kiện mạng/time, lưu tối đa 32 giao dịch vào NVS. Bản ghi giữ
cả `command_code` và `command_value` gốc nhưng hiện chưa giữ timestamp gốc;
thời gian được lấy lúc gửi lại. Queue giao dịch dùng schema version 4 với
`amount_vnd` 64-bit. Khi boot, firmware migrate schema version 3 bằng cách giữ
nguyên `command_code`/`command_value` và mở rộng `amount_vnd`; schema version 2
giữ `command_code` rồi tra mapping NVS để bổ sung `command_value`; schema version
1 dùng mode mặc định rồi thực hiện cùng phép tra.

## 9. Totalizer telemetry

Được gửi sau `get_totalizer` thành công:

Command và ACK tương ứng xem mục 6.4. Telemetry dưới đây là message thứ hai do
Node chủ động publish sau ACK; server không ACK lại telemetry này.

```json
{
  "ts": 1786523009,
  "version": "1.8",
  "data": {
    "total_amount_vnd": 125400000,
    "total_volume_l": 5016.2,
    "time_device": "13/08/2026 10:23:29"
  }
}
```

## 10. Heartbeat telemetry

Chu kỳ 60 giây:

Heartbeat không cần command kích hoạt và không có ACK từ firmware. Server theo
dõi trực tiếp payload trên topic telemetry.

```json
{
  "ts": 1786522928,
  "version": "1.8",
  "keep_alive": 1,
  "RSSI": -75,
  "data": {
    "unit_price": 25000
  }
}
```

Task heartbeat thử gửi ngay khi khởi động. Nếu MQTT chưa ready, task retry mỗi
1 giây; sau lần publish thành công, chu kỳ tiếp theo là 60 giây.

Heartbeat không bị bỏ chỉ vì SNTP, RSSI hoặc `unit_price` chưa sẵn sàng:

- time chưa valid: gửi `ts=0`;
- không đọc được RSSI hợp lệ: gửi `RSSI=0`;
- chưa từng có đơn giá hợp lệ: gửi `data.unit_price=0`;
- chỉ chưa publish được khi MQTT chưa ready, tạo JSON lỗi hoặc enqueue MQTT lỗi.

Nguồn `unit_price` là cache RAM của `pump_transaction_filter`. Cache được seed
từ NVS `device_cfg/last_price` khi boot, cập nhật khi giải mã được hàng 3 hợp lệ
hoặc khi `set_unit_price` verify thành công. NVS chỉ được ghi khi giá thay đổi.

## 11. Wi-Fi Config Portal

Giữ IO0 active-low 7 giây, debounce 50 ms. Firmware mở AP từ `WIFI_CONFIG_AP_SSID`, tối đa 4 client, channel 1.

Tính năng này được kích hoạt bằng nút IO0 và HTTP, không nhận MQTT command và
không publish ACK MQTT. Kết quả lưu/kết nối được đọc qua endpoint `/status`.

| Method | Endpoint | Chức năng |
|---|---|---|
| GET | `/` | Form nhập SSID/password |
| POST | `/save` | Validate và queue credential thử nghiệm |
| GET | `/status` | Trạng thái portal/kết nối |

SSID tối đa 32 byte; password tối đa 64 byte ở form. Credential cũ chỉ bị thay sau khi credential mới nhận được IP và ghi/readback NVS thành công.

## 12. Time manager

- Module nội bộ, không có command/ACK MQTT riêng.
- SNTP non-blocking, server `pool.ntp.org`.
- Sync khi STA có IP và re-sync mỗi giờ.
- TZ chuẩn qua `localtime_r()`, không cộng 7 thủ công.
- `time_manager_get_snapshot()` là API nên dùng khi một payload cần cả `ts` và `time_device`.

## 13. NVS layout logic

Đây là persistence nội bộ. Không có command NVS chung; từng command ở mục 6
chịu trách nhiệm ghi, readback rồi ACK theo schema của chính command đó.

| Namespace/key | Kiểu | Giới hạn |
|---|---|---|
| `wifi_cfg/credentials` | blob có magic/version | 1 credential |
| `device_cfg/core_cfg` | versioned blob | node ID + MQTT host/port/credential |
| `device_cfg/hash_lock` | u8 | boolean |
| `device_cfg/price_lock` | u8 | boolean |
| `device_cfg/last_price` | u32 | đơn giá hợp lệ gần nhất, 1..999999 VNĐ/L |
| `device_cfg/cal_mode` | string | 16 ký tự |
| `device_cfg/cal_restore` | versioned blob | mode tạm và mode cần hoàn nguyên |
| `device_cfg/shortcuts` | versioned blob | 10 slot + revision |
| `device_cfg/calib_map` | versioned blob | 5 slot |
| `device_cfg/function_keys` | versioned blob | 10 slot + revision; slot 1-5 có factory mặc định |
| `device_cfg/mqtt_trial` | versioned blob | candidate + cấu hình MQTT cũ khi thử đổi broker |
| `pump_tx/queue` | versioned blob | 32 giao dịch |

Các blob có magic/version và một số luồng migration từ version cũ. `core_cfg` được seed từ `device_config.h` nếu chưa tồn tại hoặc không hợp lệ. MQTT không log password.

Quy trình xuất xưởng:

1. Đặt factory seed riêng cho board trong `device_config.h`, đặc biệt `NODE_ID` duy nhất.
2. Build và flash firmware lần đầu.
3. Boot đầu seed `core_cfg`, Wi-Fi, lock, mode và bảng mapping rỗng vào NVS, sau đó readback.
4. Mọi thay đổi qua API/command phải commit và readback NVS trước khi báo thành công.
5. Reboot hoặc OTA app load lại NVS; factory seed mới trong binary không ghi đè cấu hình đã provision.

## 14. Phát hành OTA

Command và ACK OTA runtime xem mục 6.9. Mục này chỉ mô tả thao tác chuẩn bị
artifact ở phía phát hành firmware, không tạo thêm MQTT message.

1. Tăng `PROJECT_VER` trong root `CMakeLists.txt`.
2. Chạy `idf.py build`.
3. Chạy `python tools/prepare_ota.py --branch <OTA_RELEASE_BRANCH>`.
4. Kiểm tra version trong `OTA/version.json` trùng app version vừa build.
5. Commit/push `OTA/file.bin` và `OTA/version.json` lên đúng nhánh cấu hình bởi
   `OTA_RELEASE_BRANCH` (hiện là `feature/control_kepad`) cùng một lần.
6. Gửi command `OTA` và theo dõi log `OTA_MANAGER` qua lần reboot/self-test.

Không dùng `git@github.com:...` trong firmware; ESP32 tải bằng HTTPS Raw GitHub.

## 15. Điểm mở rộng khuyến nghị

- Bổ sung chữ ký firmware/Secure Boot để chống image bị thay thế nếu tài khoản GitHub bị xâm nhập.
- Persist dedup request nếu backend cần idempotency qua reboot.
- Lưu timestamp gốc cho buffered transaction.
- Chuyển MQTT TCP sang TLS và đưa secret ra khỏi source.
