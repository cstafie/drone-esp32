export const dynamic = "force-dynamic";

const ESP32_BASE_URL = process.env.ESP32_BASE_URL ?? "http://192.168.4.1";

export async function GET() {
  try {
    const upstream = await fetch(`${ESP32_BASE_URL}/api/v1/camera/stream`, {
      cache: "no-store",
    });

    if (!upstream.ok || !upstream.body) {
      return new Response(
        JSON.stringify({ error: "Camera stream unavailable" }),
        {
          status: 502,
          headers: { "Content-Type": "application/json" },
        },
      );
    }

    return new Response(upstream.body, {
      headers: {
        "Content-Type":
          upstream.headers.get("Content-Type") ??
          "multipart/x-mixed-replace; boundary=frame",
        "Cache-Control": "no-cache, no-store",
        "X-Accel-Buffering": "no",
      },
    });
  } catch {
    return new Response(JSON.stringify({ error: "Camera unavailable" }), {
      status: 502,
      headers: { "Content-Type": "application/json" },
    });
  }
}
