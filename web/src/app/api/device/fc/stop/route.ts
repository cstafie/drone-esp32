import { stopFcRc } from "@/lib/device-client";

export async function POST() {
  try {
    const result = await stopFcRc();
    return Response.json(result);
  } catch (err) {
    return Response.json(
      { ok: false, error: err instanceof Error ? err.message : "Device error" },
      { status: 502 },
    );
  }
}
