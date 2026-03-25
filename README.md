# ESP32 Fingerprint Attendance Device

Arduino firmware for an ESP32-based attendance terminal that uses a fingerprint sensor to record attendance and enroll students via a backend API.

## Hardware

- ESP32 microcontroller
- Adafruit fingerprint sensor (UART, RX pin 16 / TX pin 17)

## Features

- **Attendance scanning** — scans a finger, matches it against stored templates, and POSTs the result (slot ID + confidence) to the backend
- **Backend-driven enrollment** — polls the backend for pending enrollment jobs and guides the user through a two-scan enrollment; exports the template as a Base64 backup and reports success/failure back to the server
- **Retry queue** — if the enrollment result POST fails transiently, the result is queued and retried automatically every 15 seconds
- **Template export** — reads raw fingerprint template packets from the sensor and encodes them as Base64 using mbedTLS for backup storage on the server
- **WiFi auto-reconnect** — detects a dropped connection and reconnects before each operation
- **Serial command interface** — interactive menu over USB serial for manual testing

## Serial Commands

| Key | Action |
|-----|--------|
| `S` | Scan finger (attendance) |
| `P` | Force-poll backend for a pending enrollment job |
| `D` | Delete a fingerprint slot by ID |
| `C` | Count stored fingerprints |
| `H` | Health-check GET against the backend |

## Configuration

All configuration is done via `#define` macros at the top of `main/main.ino`. Override them with build flags or edit directly:

| Macro | Default | Description |
|-------|---------|-------------|
| `WIFI_SSID` | `your_wifi_ssid` | WiFi network name |
| `WIFI_PASSWORD` | `your_wifi_password` | WiFi password |
| `DEVICE_ID` | `esp32-attendance-01` | Unique device identifier sent with every request |
| `DEVICE_API_KEY` | `REPLACE_DEVICE_API_KEY` | API key sent in the `X-Device-Api-Key` header |
| `BASE_URL` | `http://192.168.1.11:8080/api` | Backend base URL |

## API Endpoints Used

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/fingerprint/devices/{deviceId}/enrollment-session` | Poll for a pending enrollment job |
| `POST` | `/fingerprint/devices/enrollment-result` | Report enrollment success or failure |
| `POST` | `/fingerprint/devices/scan` | Report a matched fingerprint scan |
| `GET` | `/health` | Backend health check |

## Dependencies

- [Adafruit Fingerprint Sensor Library](https://github.com/adafruit/Adafruit-Fingerprint-Sensor-Library)
- [ArduinoJson](https://arduinojson.org/)
- WiFi, HTTPClient, HardwareSerial (ESP32 Arduino core)
- mbedTLS Base64 (bundled with ESP32 Arduino core)

## Notes

- HTTPS is intentionally disabled during development. To enable it, uncomment the `WiFiClientSecure` sections in `main.ino`, define `TLS_CA_CERT`, and switch `BASE_URL` to `https://`.
- The device authenticates to the backend using the `X-Device-Api-Key` header on every request.
