# Remote Timer

## How to use

1. Start the app:

```bash
docker compose up -d --build
```

2. Open the web UI:

- `http://<your-server-ip>:3000`

3. Set up the device:

- Flash `remote-timer.ino` to your ESP32-C3.
- Connect to device setup page: `http://192.168.4.1`
- Enter:
  - Wi-Fi name and password
  - API host (example: `192.168.1.42:3001`)
  - API token (optional)
- Save.

4. Use the remote buttons:

- Short press: start/update timer
- Long press: delete timer

5. Use the UI:

- Edit label, notes, alert settings, mute, and order
- Save changes
- Delete single timer (keeps that timer’s settings)
- Delete all timers (also removes all settings)

## Notes

- Keep device and server on the same network.
- Data is saved and will remain after restart if your Docker volume is persistent.
- Wi-Fi behavior (current firmware):
  - Device starts STA mode and uses `WiFi.setAutoReconnect(true)`.
  - Startup attempts one `WiFi.begin(...)`, then reconnects are handled by the Wi-Fi library.
  - On startup, normal timer/button processing waits until Wi-Fi is connected, while config portal/network services keep running.
  - Serial `Connecting WiFi...` shows masked password once per boot (only last 3 chars visible).
  - Reconnect handling relies on library auto reconnect (no custom Wi-Fi event callback/recovery state machine).

## Simple Wiring Diagram

### ESP32-C3 + YK04

```
YK04 receiver                 ESP32-C3
-------------                ----------
VCC         ----------------> 3V3
GND         ----------------> GND
D0 (Button1) ----------------> GPIO0
D1 (Button2) ----------------> GPIO1
D2 (Button3) ----------------> GPIO3
D3 (Button4) ----------------> GPIO4
```

### Buzzer

```
Buzzer                       ESP32-C3
------                       ----------
+ (positive) ---------------> GPIO10
- (negative) ---------------> GND
```

Pin map above matches current firmware defaults in `remote-timer.ino`.
