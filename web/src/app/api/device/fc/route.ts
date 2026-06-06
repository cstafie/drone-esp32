import {
  getFcTelemetry,
  setFcRc,
  armFc,
  startMotorTest,
} from "@/lib/device-client";

export async function GET() {
  try {
    const fc = await getFcTelemetry();
    return Response.json({ reachable: true, fc });
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
  const body = (await request.json()) as {
    type?: string;
    channels?: number[];
    arm?: boolean;
  };

  if (body?.type === "motor-test") {
    try {
      const result = await startMotorTest();
      return Response.json(result);
    } catch (err) {
      return Response.json(
        {
          ok: false,
          error: err instanceof Error ? err.message : "Device error",
        },
        { status: 502 },
      );
    }
  }

  if (body?.type === "arm" && typeof body.arm === "boolean") {
    try {
      const result = await armFc(body.arm);
      return Response.json(result);
    } catch (err) {
      return Response.json(
        {
          ok: false,
          error: err instanceof Error ? err.message : "Device error",
        },
        { status: 502 },
      );
    }
  }

  if (body?.type !== "rc" || !Array.isArray(body.channels)) {
    return Response.json(
      { error: 'Expected {type:"rc", channels:[...]}' },
      { status: 400 },
    );
  }

  try {
    const result = await setFcRc(body.channels);
    return Response.json(result);
  } catch (err) {
    return Response.json(
      { ok: false, error: err instanceof Error ? err.message : "Device error" },
      { status: 502 },
    );
  }
}
