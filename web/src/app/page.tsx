"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";

type HealthResponse = {
  reachable: boolean;
  checkedAt: string;
  error?: string;
  device?: {
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
  };
};

type LightResponse = {
  ok: boolean;
  lightOn: boolean;
  r: number;
  g: number;
  b: number;
  error?: string;
};

function rgbToHex(r: number, g: number, b: number) {
  return "#" + [r, g, b].map((v) => v.toString(16).padStart(2, "0")).join("");
}

function hexToRgb(hex: string) {
  const n = parseInt(hex.slice(1), 16);
  return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
}

export default function Home() {
  const [health, setHealth] = useState<HealthResponse | null>(null);
  const [light, setLight] = useState<LightResponse | null>(null);
  const [cameraRunning, setCameraRunning] = useState(false);
  const [pickerColor, setPickerColor] = useState("#ff0000");
  const [pending, setPending] = useState(false);
  const [cameraPending, setCameraPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const pendingRef = useRef(false);

  const refresh = useCallback(async () => {
    if (pendingRef.current) return;
    try {
      const [healthRes, lightRes, camRes] = await Promise.all([
        fetch("/api/device/health", { cache: "no-store" }),
        fetch("/api/device/light", { cache: "no-store" }),
        fetch("/api/device/camera", { cache: "no-store" }),
      ]);

      const healthJson = (await healthRes.json()) as HealthResponse;
      const lightJson = (await lightRes.json()) as LightResponse;
      const camJson = (await camRes.json()) as {
        reachable: boolean;
        camera?: { running: boolean };
      };

      setHealth(healthJson);
      if (lightJson.ok) setLight(lightJson);
      if (camJson.reachable && camJson.camera)
        setCameraRunning(camJson.camera.running);
      setError(
        healthJson.reachable ? null : (healthJson.error ?? "ESP32 unreachable"),
      );
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to reach device");
    }
  }, []);

  useEffect(() => {
    queueMicrotask(() => {
      void refresh();
    });
    const timer = setInterval(() => {
      void refresh();
    }, 3000);
    return () => clearInterval(timer);
  }, [refresh]);

  const sendColor = useCallback(async (r: number, g: number, b: number) => {
    setPending(true);
    pendingRef.current = true;
    setError(null);
    try {
      const res = await fetch("/api/device/light", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ r, g, b }),
      });
      const result = (await res.json()) as LightResponse;
      if (!res.ok || !result.ok) throw new Error(result.error ?? "Failed");
      setLight(result);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to set color");
    } finally {
      setPending(false);
      pendingRef.current = false;
    }
  }, []);

  const sendOff = useCallback(async () => {
    setPending(true);
    pendingRef.current = true;
    setError(null);
    try {
      const res = await fetch("/api/device/light", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ state: "off" }),
      });
      const result = (await res.json()) as LightResponse;
      if (!res.ok || !result.ok) throw new Error(result.error ?? "Failed");
      setLight(result);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to turn off");
    } finally {
      setPending(false);
      pendingRef.current = false;
    }
  }, []);

  const toggleCamera = useCallback(async () => {
    setCameraPending(true);
    setError(null);
    try {
      const action = cameraRunning ? "stop" : "start";
      const res = await fetch("/api/device/camera", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action }),
      });
      const result = (await res.json()) as {
        ok: boolean;
        running: boolean;
        error?: string;
      };
      if (!res.ok || !result.ok)
        throw new Error(result.error ?? "Camera toggle failed");
      setCameraRunning(result.running);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Camera error");
    } finally {
      setCameraPending(false);
    }
  }, [cameraRunning]);

  const statusClass = useMemo(
    () =>
      health?.reachable ? "status-pill connected" : "status-pill disconnected",
    [health],
  );

  const currentHex =
    light && light.lightOn ? rgbToHex(light.r, light.g, light.b) : null;

  return (
    <div className="flex flex-1 items-center justify-center p-4 md:p-8">
      <main className="panel w-full max-w-4xl p-6 md:p-10">
        <div className="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p className="text-sm uppercase tracking-[0.2em] text-(--muted)">
              Drone ESP32 Ground Link
            </p>
            <h1 className="mt-2 text-3xl font-semibold md:text-4xl">
              Control Panel
            </h1>
          </div>
          <span className={statusClass}>
            {health?.reachable ? "Connected" : "Disconnected"}
          </span>
        </div>

        {/* Stats row */}
        <section className="mt-8 grid gap-4 md:grid-cols-3">
          <div className="rounded-2xl bg-(--surface) p-4">
            <p className="text-xs uppercase tracking-[0.16em] text-(--muted)">
              LED
            </p>
            <div className="mt-2 flex items-center gap-3">
              <span
                className="inline-block h-6 w-6 rounded-full border border-black/10"
                style={{
                  background: currentHex ?? "#1b2116",
                  opacity: currentHex ? 1 : 0.2,
                }}
              />
              <p className="text-xl font-medium">
                {light === null ? "--" : light.lightOn ? currentHex : "Off"}
              </p>
            </div>
          </div>
          <div className="rounded-2xl bg-(--surface) p-4">
            <p className="text-xs uppercase tracking-[0.16em] text-(--muted)">
              Signal
            </p>
            <p className="mt-2 text-xl font-medium">
              {health?.device ? `${health.device.wifiRssiDbm} dBm` : "--"}
            </p>
          </div>
          <div className="rounded-2xl bg-(--surface) p-4">
            <p className="text-xs uppercase tracking-[0.16em] text-(--muted)">
              Mode
            </p>
            <p className="mt-2 text-xl font-medium">
              {health?.device?.mode ?? "--"}
            </p>
          </div>
        </section>

        {/* Color control */}
        <section className="mt-6 rounded-2xl bg-(--surface) p-5">
          <p className="text-xs uppercase tracking-[0.16em] text-(--muted)">
            RGB LED Control
          </p>
          <div className="mt-4 flex flex-wrap items-center gap-4">
            <input
              type="color"
              value={pickerColor}
              onChange={(e) => setPickerColor(e.target.value)}
              className="h-12 w-16 cursor-pointer rounded-xl border border-black/10 p-1"
              disabled={pending || !health?.reachable}
            />
            <button
              type="button"
              className="btn btn-primary"
              disabled={pending || !health?.reachable}
              onClick={() => {
                const { r, g, b } = hexToRgb(pickerColor);
                void sendColor(r, g, b);
              }}
            >
              Set Color
            </button>
            <button
              type="button"
              className="btn btn-secondary"
              disabled={pending || !health?.reachable}
              onClick={() => void sendOff()}
            >
              Off
            </button>
            <button
              type="button"
              className="btn btn-secondary"
              disabled={pending}
              onClick={() => void refresh()}
            >
              Refresh
            </button>
          </div>
        </section>

        {/* Camera */}
        <section className="mt-6 rounded-2xl bg-(--surface) p-5">
          <div className="flex items-center justify-between gap-4">
            <p className="text-xs uppercase tracking-[0.16em] text-(--muted)">
              Camera
            </p>
            <span
              className={
                cameraRunning
                  ? "status-pill connected"
                  : "status-pill disconnected"
              }
            >
              {cameraRunning ? "Live" : "Off"}
            </span>
          </div>
          <div className="mt-4">
            <button
              type="button"
              className={
                cameraRunning ? "btn btn-secondary" : "btn btn-primary"
              }
              disabled={cameraPending || !health?.reachable}
              onClick={() => void toggleCamera()}
            >
              {cameraPending
                ? "…"
                : cameraRunning
                  ? "Stop Camera"
                  : "Start Camera"}
            </button>
          </div>
          {cameraRunning && (
            <div className="mt-4 overflow-hidden rounded-xl border border-black/10">
              {/* eslint-disable-next-line @next/next/no-img-element */}
              <img
                src="/api/device/camera/stream"
                alt="Camera feed"
                className="w-full"
                onError={() => setCameraRunning(false)}
              />
            </div>
          )}
        </section>

        {/* Diagnostics */}
        <section className="mt-4 rounded-2xl bg-(--surface) p-4">
          <p className="text-xs uppercase tracking-[0.16em] text-(--muted)">
            Diagnostics
          </p>
          <p className="mono mt-2 text-sm">
            Last check: {health?.checkedAt ?? "--"}
          </p>
          <p className="mono mt-1 text-sm">
            ESP32 IP: {health?.device?.ip ?? "--"}
          </p>
          <p className="mono mt-1 text-sm">
            Uptime:{" "}
            {health?.device
              ? `${Math.floor(health.device.uptimeMs / 1000)}s`
              : "--"}
          </p>
          {error ? (
            <p className="mono mt-2 text-sm text-[#922f17]">Error: {error}</p>
          ) : null}
        </section>
      </main>
    </div>
  );
}
