## Web Control App

This Next.js app runs on localhost and acts as the operator UI.
It never talks to the ESP32 directly from the browser. Instead, it uses local API routes in this app as a proxy layer.

## Environment

Create `web/.env.local` using `web/.env.example` as a base:

```bash
ESP32_BASE_URL=http://192.168.1.73
ESP32_DEVICE_TOKEN=change-me
```

- `ESP32_BASE_URL`: IP and scheme for the device.
- `ESP32_DEVICE_TOKEN`: Optional shared secret sent in `X-Device-Token`.

## Run Locally

```bash
cd web
npm run dev
```

Then open [http://localhost:3000](http://localhost:3000).

## API Contract Used by the UI

- `GET /api/device/health` -> checks whether the ESP32 is reachable.
- `GET /api/device/light` -> returns light status.
- `POST /api/device/light` with JSON `{ "state": "on" | "off" }` -> sets light state.

The Next.js API routes call the device firmware endpoints:

- `GET /api/v1/health`
- `GET /api/v1/light/status`
- `POST /api/v1/light/start`
- `POST /api/v1/light/stop`

## Why Proxy Through Next.js

- Keeps browser code simple and stable even if device APIs evolve.
- Lets you centralize auth, retries, logging, and future telemetry upload.
- Avoids CORS issues and creates a clean seam for future multi-device support.
