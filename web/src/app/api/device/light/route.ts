import { NextRequest, NextResponse } from "next/server";
import { getLightStatus, setLightColor, stopLight } from "@/lib/device-client";

export const runtime = "nodejs";

export async function GET() {
  try {
    const status = await getLightStatus();
    return NextResponse.json(status, { status: 200 });
  } catch (error) {
    const message = error instanceof Error ? error.message : "Unknown error";
    return NextResponse.json({ ok: false, error: message }, { status: 502 });
  }
}

export async function POST(request: NextRequest) {
  let body: unknown;

  try {
    body = await request.json();
  } catch {
    return NextResponse.json(
      { ok: false, error: "Invalid JSON body" },
      { status: 400 },
    );
  }

  if (typeof body !== "object" || body === null) {
    return NextResponse.json(
      { ok: false, error: "Body must be a JSON object" },
      { status: 400 },
    );
  }

  const b = body as Record<string, unknown>;

  // Off: { state: "off" }
  if (b.state === "off") {
    try {
      return NextResponse.json(await stopLight(), { status: 200 });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return NextResponse.json({ ok: false, error: message }, { status: 502 });
    }
  }

  // Color: { r: 0-255, g: 0-255, b: 0-255 }
  const r =
    typeof b.r === "number"
      ? Math.round(Math.min(255, Math.max(0, b.r)))
      : null;
  const g =
    typeof b.g === "number"
      ? Math.round(Math.min(255, Math.max(0, b.g)))
      : null;
  const bv =
    typeof b.b === "number"
      ? Math.round(Math.min(255, Math.max(0, b.b)))
      : null;

  if (r === null || g === null || bv === null) {
    return NextResponse.json(
      { ok: false, error: "Body must include r, g, b (0-255) or state: 'off'" },
      { status: 400 },
    );
  }

  try {
    return NextResponse.json(await setLightColor(r, g, bv), { status: 200 });
  } catch (error) {
    const message = error instanceof Error ? error.message : "Unknown error";
    return NextResponse.json({ ok: false, error: message }, { status: 502 });
  }
}
