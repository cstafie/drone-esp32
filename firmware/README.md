# ESP32 Firmware

This firmware exposes a tiny HTTP API for local control from the web app.

## Setup

1. Copy `include/config.h.example` to `include/config.h`.
2. Fill in Wi-Fi credentials and optional device token.
3. Build and flash:

```bash
cd firmware
pio run -t upload
pio device monitor
```

## Using Environment Variables

On embedded firmware, these values are compile-time settings (not runtime OS env vars).

`include/config.h` supports macro overrides such as `WIFI_SSID_VALUE` and `DEVICE_TOKEN_VALUE`.
If you want to inject them from your shell, uncomment the example `build_flags` lines in `platformio.ini`, then run with env vars set.

Git Bash example:

```bash
export WIFI_SSID="your-wifi-name"
export WIFI_PASSWORD="your-wifi-password"
export DEVICE_TOKEN="change-me"
pio run -t upload
```

## Endpoints

- `GET /api/v1/health`
- `GET /api/v1/light/status`
- `POST /api/v1/light/start`
- `POST /api/v1/light/stop`
- `POST /api/v1/fc/uart` (stub for future UART bridge)

## Networking Behavior

- Tries station mode first with configured Wi-Fi.
- Falls back to AP mode if station connect fails.

## Security

If `DEVICE_TOKEN` is non-empty, mutation endpoints require header:

- `X-Device-Token: <token>`
