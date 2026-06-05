export interface DeviceHealth {
  ok: boolean;
  uptimeMs: number;
  wifiRssiDbm: number;
  lightOn: boolean;
  r: number;
  g: number;
  b: number;
  cameraOn: boolean;
  mode: "station" | "access-point";
  ip: string;
}

export interface LightStatus {
  ok: boolean;
  lightOn: boolean;
  r: number;
  g: number;
  b: number;
}

const ESP32_BASE_URL = process.env.ESP32_BASE_URL ?? "http://192.168.1.73";

async function requestEsp32<T>(path: string, init?: RequestInit): Promise<T> {
  const headers = new Headers(init?.headers);
  headers.set("Content-Type", "application/json");

  const response = await fetch(`${ESP32_BASE_URL}${path}`, {
    ...init,
    headers,
    cache: "no-store",
    next: { revalidate: 0 },
  });

  if (!response.ok) {
    const body = await response.text();
    throw new Error(`ESP32 request failed (${response.status}): ${body}`);
  }

  return (await response.json()) as T;
}

export function getHealth() {
  return requestEsp32<DeviceHealth>("/api/v1/health");
}

export function getLightStatus() {
  return requestEsp32<LightStatus>("/api/v1/light/status");
}

export function setLightColor(r: number, g: number, b: number) {
  return requestEsp32<LightStatus>("/api/v1/light/color", {
    method: "POST",
    body: JSON.stringify({ r, g, b }),
  });
}

export function stopLight() {
  return requestEsp32<LightStatus>("/api/v1/light/stop", {
    method: "POST",
  });
}

export interface CameraStatus {
  ok: boolean;
  running: boolean;
}

export function getCameraStatus() {
  return requestEsp32<CameraStatus>("/api/v1/camera/status");
}

export function startCamera() {
  return requestEsp32<CameraStatus>("/api/v1/camera/start", { method: "POST" });
}

export function stopCamera() {
  return requestEsp32<CameraStatus>("/api/v1/camera/stop", { method: "POST" });
}
