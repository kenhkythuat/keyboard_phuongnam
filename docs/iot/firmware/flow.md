# Firmware Runtime Flows

## 1. Boot và network

```mermaid
sequenceDiagram
    participant Boot as app_main
    participant Settings as device_settings
    participant Key as Virtual key + LED sniffer
    participant Calibration as calibration flow
    participant WiFi as wifi_manager
    participant Time as time_manager
    participant MQTT as mqtt_manager
    participant App as Application tasks

    Boot->>Boot: init NVS
    Boot->>Settings: init()
    alt core_cfg chưa có/không hợp lệ
        Settings->>Settings: seed factory core config + readback
    else core_cfg hợp lệ
        Settings->>Settings: load node/MQTT config + command settings
    end
    Boot->>Key: start sniffer + init virtual key GPIO
    Boot->>Calibration: load cal_mode và mapping từ NVS
    Calibration->>Key: áp dụng lại raw_command, verify tối đa 3 lần
    Calibration-->>Boot: success hoặc log lỗi và tiếp tục
    Boot->>WiFi: start()
    WiFi->>WiFi: load credentials và connect STA
    WiFi-->>Boot: return ngay
    Boot->>Time: start SNTP manager
    Boot->>MQTT: init client bằng core_cfg từ NVS
    Boot->>App: start heartbeat/P88/status LED/keypad scan
    WiFi-->>Time: IP_EVENT_STA_GOT_IP
    Time->>Time: start/restart SNTP
    WiFi-->>MQTT: IP_EVENT_STA_GOT_IP
    MQTT->>MQTT: connect broker
    MQTT->>MQTT: subscribe command topic
    MQTT->>MQTT: set MQTT_READY
```

Không bước nào buộc `app_main()` chờ network. Khi Wi-Fi mất, MQTT ready bị clear; Wi-Fi và MQTT tự reconnect bằng timer/backoff.

OTA app chỉ thay nội dung `ota_0`/`ota_1`. Partition `nvs` riêng được giữ nguyên, nên boot sau OTA đi theo nhánh load cấu hình đã provision thay vì seed lại.

## 2. Sniffer LED

```mermaid
flowchart TD
    A[External MBI bus] --> B[SPI slave capture]
    B --> C{192 bits?}
    C -- No --> D[Increment other counter]
    C -- Yes --> E[Decode 6 blocks]
    E --> F[Confirm each column twice]
    F --> G{Enough 6 columns?}
    G -- No --> B
    G -- Yes --> H[Confirm full display twice]
    H --> I{Different from latest display?}
    I -- No --> B
    I -- Yes --> J[Update snapshot and generation]
    J --> K[Log COL/LED]
    J --> L[Submit to transaction filter]
    J --> M[Mirror to LED buffer when external mode]
```

`s_display_observation_generation` tăng khi có một màn hình hoàn chỉnh được quan sát. `s_display_generation` và log chi tiết chỉ tăng/in khi nội dung đổi.

## 3. Nhận biết giao dịch bơm

```mermaid
flowchart TD
    A[Stable decoded display] --> B[Parse 3 numeric rows]
    B --> C{amount=0, volume=0, price>0?}
    C -- Yes --> D[Start transaction and capture mode_calibration]
    C -- No --> E{Transaction active?}
    E -- No --> F[Remember latest idle positive display]
    E -- Yes --> G{amount and volume positive?}
    G -- No --> H[Wait next display]
    G -- Yes --> I{First positive sample?}
    I -- Yes --> J{Same as display before zero?}
    J -- Yes --> K[Treat as restored old sale]
    J -- No --> L[Accept as new candidate]
    I -- No --> M[Check same price and non-decreasing values]
    M --> N[Update latest candidate]
    L --> O{No change for 10 seconds?}
    N --> O
    O -- No --> H
    O -- Yes --> P[Validate amount formula]
    P --> Q{Time, RSSI, MQTT ready?}
    Q -- Yes --> R[Publish transaction telemetry]
    Q -- No --> S[Append NVS pending queue]
```

Nếu firmware bắt đầu khi màn hình đã ở `0/0`, không có màn hình cũ làm mốc, mẫu dương đầu tiên vẫn phải có thêm một bước tăng mới được coi là giao dịch.

## 4. Buffered transaction

```mermaid
sequenceDiagram
    participant Filter as Pump filter
    participant NVS as pump_tx/queue
    participant Time as time_manager
    participant MQTT as mqtt_manager

    Filter->>Filter: transaction complete
    Filter->>Time: get snapshot
    Time-->>Filter: invalid / network not ready
    Filter->>NVS: append transaction + command_code
    loop mỗi 1 giây khi có pending
        Filter->>Time: is_valid()
        Filter->>NVS: peek oldest
        Filter->>MQTT: publish is_buffered=true
        alt publish queued
            Filter->>NVS: pop oldest
        else chưa gửi được
            Filter->>NVS: giữ nguyên
        end
    end
```

Queue FIFO tối đa 32 phần tử. Bản ghi hiện lưu tiền, lít, đơn giá và command code, chưa lưu timestamp gốc.

## 5. MQTT command

```mermaid
sequenceDiagram
    participant Server
    participant MQTT as mqtt_manager callback
    participant Queue as command queue
    participant Task as mqtt_command task
    participant Device as Virtual key / LED / NVS

    Server->>MQTT: publish command QoS 1
    MQTT->>MQTT: verify exact command topic
    MQTT->>Queue: copy payload, non-blocking
    Queue->>Task: receive
    Task->>Task: wait time valid and MQTT ready
    Task->>Task: parse envelope and deduplicate request_id
    Task->>Device: execute command/readback
    Device-->>Task: applied state
    Task->>Server: publish ACK QoS 1
```

Command callback không chạy chuỗi phím. Công việc dài nằm trong command task.

## 6. Get Totalizer

```mermaid
flowchart TD
    A[get_totalizer or P88] --> B[Lock totalizer mutex]
    B --> C[Send C#7733]
    C --> D[Capture baseline generation]
    D --> E[Send E]
    E --> F{Fresh frame has P20?}
    F -- No, under 3 attempts --> C
    F -- Yes --> G[Decode row 2 as total_amount_vnd]
    G --> H[Send C]
    H --> I[Send C#44504ET]
    I --> J[Wait 1 second]
    J --> K{Fresh volume frame valid?}
    K -- No, under 3 attempts --> I
    K -- Yes --> L[Decode total_volume_l]
    L --> M[Send C and unlock]
    M --> N{Source?}
    N -- MQTT --> O[ACK + totalizer telemetry]
    N -- P88 --> P[IO39=1, amount row 1, volume row 3]
```

Trong P88, bất kỳ phím vật lý nào yêu cầu thoát; firmware trả IO39 về 0 và không dùng phím thoát để kích hoạt command khác.

## 7. Set unit price

```mermaid
flowchart TD
    A[Validate unit_price] --> B[ACK unit_price_accepted]
    B --> C[Send #44504EP + price]
    C --> D[Capture generation]
    D --> E[Send E]
    E --> F{Fresh LED: row1 E0 and row3=price?}
    F -- Yes --> G[Send C, complete]
    F -- No and attempts < 3 --> C
    F -- No after 3 --> H[Log failure]
```

ACK hiện biểu thị command đã được tiếp nhận, chưa biểu thị thao tác cuối chắc chắn thành công.

## 8. Calibration

```mermaid
flowchart TD
    A[MQTT indexed slot or local shortcut] --> B[Lock totalizer mutex]
    B --> C[Send C#122973E]
    C --> D{Fresh row1=P06?}
    D -- No --> E[Send C and retry, max 3]
    D -- Yes --> F[Wait 1000 ms]
    F --> G[Send raw_command]
    G --> H{Fresh row2 equals raw command?}
    H -- No --> E
    H -- Yes --> I[Send E]
    I --> J{Fresh row1=E0 and row2 matches?}
    J -- No --> E
    J -- Yes --> K[Send C]
    K --> L{Source MQTT?}
    L -- Yes --> M[Save slot and mode_calibration, ACK]
    L -- No --> N[Update mode_calibration, no ACK]
```

Virtual output được phân biệt với physical input nên không tự kích hoạt shortcut lặp vô hạn.

## 9. Wi-Fi Config Mode

```mermaid
sequenceDiagram
    participant User
    participant Button as IO0 task
    participant Portal as SoftAP/Web
    participant WiFi as Wi-Fi manager
    participant NVS
    participant LED

    User->>Button: hold IO0 > 7 s
    Button->>Portal: start APSTA + HTTP server
    Portal->>LED: show CONFIG, IO39=1
    User->>Portal: POST /save SSID/password
    Portal->>WiFi: queue trial credentials
    WiFi->>WiFi: connect without saving
    alt got IP
        WiFi->>NVS: save and readback credential
        WiFi->>Portal: stop AP, return STA
        WiFi->>LED: show donE for 5 s
    else failed
        WiFi->>WiFi: restore old credential
        WiFi->>LED: show FAiL for 5 s
    end
    LED->>LED: IO39=0, return external display control
```

## 10. SNTP

```mermaid
flowchart TD
    A[Got IP] --> B[Start/restart SNTP non-blocking]
    B --> C{Sync callback received?}
    C -- Yes --> D[Set TIME_VALID]
    D --> E[Use ESP-IDF system time]
    E --> F[Automatic re-sync each hour]
    C -- No --> G[Keep firmware running]
    G --> A
```

## 11. OTA

```mermaid
sequenceDiagram
    participant Server
    participant Command as MQTT command task
    participant OTA as ota_manager task
    participant GitHub as GitHub Raw HTTPS
    participant Boot as Bootloader

    Server->>Command: cmd=OTA, param={}
    Command->>OTA: queue update request
    Command-->>Server: ACK ok / ota_accepted
    OTA->>GitHub: GET OTA/version.json
    OTA->>OTA: compare remote version > current version
    alt version mới hơn
        OTA->>GitHub: GET OTA/file.bin
        OTA->>OTA: verify image project + version
        OTA->>OTA: write inactive OTA slot
        OTA->>Boot: reboot into PENDING_VERIFY image
        Boot->>OTA: start new firmware
        OTA->>OTA: wait 10 s + verify core NVS
        OTA->>Boot: mark app valid
    else bằng hoặc thấp hơn
        OTA->>OTA: log up-to-date, không ghi flash
    end
```

Nếu firmware mới reset/crash trước khi gọi mark-valid, bootloader chọn lại app slot hợp lệ trước đó. NVS không nằm trong app slot nên cấu hình thiết bị không bị thay bằng factory seed sau OTA.
