# Appsmith UI + n8n API/webhook + ESP32-C3 YK04 RF

UI - Remote-Timers.json (import to Appsmith)

API - multi-timer.json (import to n8n)

device/button - remote-timer.ino (upload via Arduino to ESP32-C3 board)

# ESP32-C3 Remote Timer (YK04 + Buzzer)

This project runs on an ESP32-C3 and uses a YK04 4-button RF receiver.

- Short press on a button: `POST /webhook/timer?id=<deviceId>:<button>`
- Long press on a button: `DELETE /webhook/timer?id=<deviceId>:<button>`
- No request body is sent for either method.

`deviceId` is generated from the ESP32-C3 eFuse MAC (stable per device).

## File

- `remote-timer.ino`

## Hardware

- ESP32-C3 board
- YK04 receiver module (4 digital outputs)
- Buzzer (active or passive)

## Wiring

Update pins in `remote-timer.ino` for your board/wiring.

Current defaults:

- YK04 buttons -> GPIO `2`, `3`, `4`, `5`
- Buzzer -> GPIO `10`

Notes:

- Code assumes YK04 output is **HIGH while pressed**.
- If your receiver is inverted, change `digitalRead(...) == HIGH` accordingly.

## Build / Flash

1. Open `remote-timer.ino` in Arduino IDE (or PlatformIO).
2. Install/select an ESP32 board package.
3. Choose an ESP32-C3 board profile matching your hardware.
4. Compile and upload.
5. Open Serial Monitor at `115200` baud.

## First-Time Setup (Wi-Fi + API)

On boot, device starts AP mode for config portal.

- AP SSID format: `RemoteTimer-XXXXXX`
- Connect to that AP.
- Open `http://192.168.4.1/`
- Fill in:
  - Wi-Fi SSID/password
  - Timer Webhook URL (default: `http://192.168.1.101:30109/webhook/timer`)
  - Optional Bearer token
- Save.

Settings are stored in ESP32 NVS (`Preferences`) and persist across reboot.

## API Behavior

Given button `N` (1..4), device calls:

- Short press: `POST <webhookUrl>?id=<deviceId>:N`
- Long press: `DELETE <webhookUrl>?id=<deviceId>:N`

Example:

- `POST http://192.168.1.101:30109/webhook/timer?id=ABCDEF123456:1`
- `DELETE http://192.168.1.101:30109/webhook/timer?id=ABCDEF123456:1`

If configured, header is included:

- `Authorization: Bearer <token>`

## Current Timing / UX

- Debounce: `100 ms`
- Long press threshold: `1200 ms`
- Main loop delay: `50 ms`
- Button press tone duration: `90 ms`
- Per-button tones:
  - Button 1: `523 Hz`
  - Button 2: `659 Hz`
  - Button 3: `784 Hz`
  - Button 4: `988 Hz`

## Tuning

Edit constants near top of `remote-timer.ino`:

- `BUTTON_PINS`
- `BUZZER_PIN`
- `DEBOUNCE_MS`
- `LONG_PRESS_MS`
- `TONE_MS`
- `BUTTON_TONES`
- `WIFI_CONNECT_TIMEOUT`, `WIFI_RETRY_INTERVAL`

## Troubleshooting

- If API calls fail, check Serial Monitor for HTTP status and error text.
- If no press detected, verify YK04 output voltage/polarity and shared ground.
- If buzzer is silent, verify buzzer type and pin; adjust `BUZZER_PIN`.
- If Wi-Fi does not connect, re-open portal AP and update credentials.
