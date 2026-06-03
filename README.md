# Drone ESP32 Project Bootstrap

This repository is structured to move from simple Wi-Fi device control into a full drone bridge stack.

## Architecture (Now -> Next)

1. Operator UI (`web/`)
- Next.js app on `localhost:3000`.
- Provides dashboard + buttons for commands.
- Uses local API routes as a backend-for-frontend layer.

2. Device Firmware (`firmware/`)
- ESP32 firmware (Arduino via PlatformIO).
- Exposes minimal HTTP API over Wi-Fi.
- Owns hardware actions (GPIO light today, UART + camera hooks later).

3. Control Plane Contract
- Keep the device API small and versioned (`/api/v1/*`).
- Web app talks only to contract, not internals.
- This lets firmware and UI evolve independently.

## Why This Is the Right Shape

- Simple now: one localhost web app + one ESP32 firmware.
- Correct boundaries: UI state in web, hardware control on device.
- Scales later:
  - Add MJPEG/WebRTC camera stream endpoint.
  - Add UART command queue to flight controller.
  - Add websocket/SSE telemetry endpoint.
  - Add safety interlocks in firmware before arming or mode changes.

## Project Layout

- `web/`: Next.js control panel and proxy API.
- `firmware/`: PlatformIO ESP32 firmware.

## Quick Start

### 1) Firmware

1. Install PlatformIO (VS Code extension or CLI).
2. Copy `firmware/include/config.h.example` -> `firmware/include/config.h`.
3. Set Wi-Fi credentials and token.
4. Build and flash:

```bash
cd firmware
pio run -t upload
pio device monitor
```

### 2) Web App

1. Copy `web/.env.example` -> `web/.env.local`.
2. Set `ESP32_BASE_URL` and matching token.
3. Start app:

```bash
cd web
npm run dev
```

Open http://localhost:3000 and use Start/Stop Light.

## Next Milestones

1. Add telemetry stream (`/api/v1/telemetry`) from ESP32 and live graph on web.
2. Add FC UART command gateway with checksum/ack and command queue.
3. Add camera endpoint and dashboard view.
4. Add safety state machine (disarmed/armed/failsafe) in firmware.
# drone-esp32
