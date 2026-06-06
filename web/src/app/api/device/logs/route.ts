import { NextResponse } from "next/server";
import { getLogs } from "@/lib/device-client";

export const runtime = "nodejs";

export async function GET() {
  try {
    const data = await getLogs();
    return NextResponse.json(
      {
        reachable: true,
        checkedAt: new Date().toISOString(),
        ...data,
      },
      { status: 200 },
    );
  } catch (error) {
    const message = error instanceof Error ? error.message : "Unknown error";
    return NextResponse.json(
      {
        reachable: false,
        checkedAt: new Date().toISOString(),
        logs: [],
        error: message,
      },
      { status: 502 },
    );
  }
}
