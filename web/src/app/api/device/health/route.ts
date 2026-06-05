import { NextResponse } from "next/server";
import { getHealth } from "@/lib/device-client";

export const runtime = "nodejs";

export async function GET() {
  try {
    const device = await getHealth();

    return NextResponse.json(
      {
        reachable: true,
        checkedAt: new Date().toISOString(),
        device,
      },
      { status: 200 },
    );
  } catch (error) {
    const message = error instanceof Error ? error.message : "Unknown error";

    return NextResponse.json(
      {
        reachable: false,
        checkedAt: new Date().toISOString(),
        error: message,
      },
      { status: 502 },
    );
  }
}
