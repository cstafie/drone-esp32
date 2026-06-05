import { getCameraStatus, startCamera, stopCamera } from "@/lib/device-client";

export async function GET() {
  try {
    const status = await getCameraStatus();
    return Response.json({ reachable: true, camera: status });
  } catch (err) {
    return Response.json(
      {
        reachable: false,
        error: err instanceof Error ? err.message : "Unreachable",
      },
      { status: 502 },
    );
  }
}

export async function POST(request: Request) {
  const body = (await request.json()) as { action?: string };
  const action = body?.action;

  if (action !== "start" && action !== "stop") {
    return Response.json(
      { error: 'action must be "start" or "stop"' },
      { status: 400 },
    );
  }

  try {
    const result =
      action === "start" ? await startCamera() : await stopCamera();
    return Response.json(result);
  } catch (err) {
    return Response.json(
      { ok: false, error: err instanceof Error ? err.message : "Device error" },
      { status: 502 },
    );
  }
}
