"""
Fake SSE server that mimics the Monster Book of Monsters battlebot.

Broadcasts telemetry in the same CSV format as the real bot's web_server.c,
so the bridge can be developed and demoed without physical hardware.

Usage:
    uv run python fake_sse_server.py [--port 7002]

Then connect at:
    http://localhost:7002/          (dashboard placeholder)
    http://localhost:7002/events    (SSE stream)
"""

import argparse
import asyncio
import math
import random
import time

from aiohttp import web

# Match the bot's SSE format from web_server.c:generate_sse_frame()
# data: STATE,failsafe,left,right,weapon,age_ms,roll,pitch,yaw,
#       test_active,test_motor,test_power,test_remaining_ms,temp_c,humidity

SSE_INTERVAL_SEC = 0.2  # 200ms, matches WEB_SERVER_SSE_INTERVAL_MS


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def noise(amp: float) -> float:
    return (random.random() - 0.5) * 2 * amp


class BotSimulator:
    """Simulates battlebot telemetry with realistic dynamics."""

    def __init__(self) -> None:
        self.start_time = time.monotonic()
        self.left = 0.0
        self.right = 0.0
        self.weapon = 0.0
        self.roll = 0.0
        self.pitch = 0.0
        self.yaw = 0.0
        self.battery_v = 12.4
        self.temp_c = 28.0
        self.humidity = 48.0
        self.state = "INIT"
        self.failsafe = False

    def step(self) -> str:
        t = time.monotonic() - self.start_time

        # Cycle through phases to make the demo interesting
        phase_t = t % 60  # 60-second repeating cycle

        if phase_t < 3:
            # Init
            self.state = "INIT" if phase_t < 1.5 else "ACTIVE"
            self.left = 0
            self.right = 0
            self.weapon = 0
        elif phase_t < 15:
            # Driving around
            self.state = "ACTIVE"
            self.left = math.sin(t * 0.7) * 50 + noise(5)
            self.right = math.sin(t * 0.7 + 0.3) * 45 + noise(5)
            self.weapon = 0
        elif phase_t < 20:
            # Weapon spinup
            frac = (phase_t - 15) / 5
            self.weapon = frac * 85 + noise(3)
            self.left = 30 + noise(5)
            self.right = 28 + noise(5)
        elif phase_t < 35:
            # Combat - aggressive driving with weapon
            self.left = clamp(math.sin(t * 1.2) * 80 + noise(10), -100, 100)
            self.right = clamp(math.cos(t * 0.9) * 75 + noise(10), -100, 100)
            self.weapon = 90 + noise(5)
        elif phase_t < 38:
            # Hit! IMU spike
            decay = math.exp(-(phase_t - 35) * 2)
            self.left = noise(40) * decay
            self.right = noise(40) * decay
            self.roll = 30 * decay * math.sin(t * 12)
            self.pitch = 20 * decay * math.cos(t * 10)
            self.yaw += 15 * decay
            self.weapon = max(0, self.weapon - 20)
        elif phase_t < 50:
            # Recovery and continued fighting
            self.left = math.sin(t * 0.6) * 60 + noise(8)
            self.right = math.sin(t * 0.6 + 0.2) * 55 + noise(8)
            self.weapon = clamp(self.weapon + 2, 0, 80) + noise(3)
            self.roll *= 0.9
            self.pitch *= 0.9
        else:
            # Wind down
            frac = (phase_t - 50) / 10
            self.left *= 0.85
            self.right *= 0.85
            self.weapon *= 0.9
            if phase_t > 57:
                self.state = "STOPPED"

        # Steady-state dynamics
        self.roll = self.roll * 0.95 + noise(0.5)
        self.pitch = self.pitch * 0.95 + noise(0.5)
        self.yaw += (self.left - self.right) * 0.002 + noise(0.2)

        # Battery sag under load
        load = (abs(self.left) + abs(self.right) + self.weapon * 1.5) / 350
        self.battery_v = clamp(12.4 - t * 0.001 - load * 0.3 + noise(0.02), 10.0, 12.6)

        # Temperature slowly climbs
        self.temp_c = clamp(28 + t * 0.02 + load * 5 + noise(0.1), 20, 55)
        self.humidity = clamp(48 - t * 0.01 + noise(0.2), 30, 55)

        self.failsafe = False
        age_ms = int(10 + random.random() * 30)

        # Format exactly like web_server.c:generate_sse_frame()
        return (
            f"data: {self.state},"
            f"{1 if self.failsafe else 0},"
            f"{int(self.left)},{int(self.right)},{int(self.weapon)},"
            f"{age_ms},"
            f"{self.roll:.1f},{self.pitch:.1f},{self.yaw:.1f},"
            f"0,none,0,0,"
            f"{int(self.temp_c)},{int(self.humidity)}"
            f"\n\n"
        )


async def sse_handler(request: web.Request) -> web.StreamResponse:
    sim: BotSimulator = request.app["simulator"]

    response = web.StreamResponse(
        status=200,
        headers={
            "Content-Type": "text/event-stream",
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
        },
    )
    await response.prepare(request)
    await response.write(b"retry: 1000\n\n")

    print(f"[SSE] Client connected from {request.remote}")
    try:
        while True:
            frame = sim.step()
            await response.write(frame.encode())
            await asyncio.sleep(SSE_INTERVAL_SEC)
    except (ConnectionResetError, asyncio.CancelledError):
        print(f"[SSE] Client disconnected from {request.remote}")
    return response


async def index_handler(request: web.Request) -> web.Response:
    return web.Response(
        text="<h1>Monster Book of Monsters (Fake)</h1>"
        "<p>SSE stream at <a href='/events'>/events</a></p>",
        content_type="text/html",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Fake battlebot SSE server")
    parser.add_argument("--port", type=int, default=7002)
    parser.add_argument("--host", type=str, default="0.0.0.0")
    args = parser.parse_args()

    app = web.Application()
    app["simulator"] = BotSimulator()
    app.router.add_get("/", index_handler)
    app.router.add_get("/events", sse_handler)

    print(f"Fake battlebot SSE server at http://{args.host}:{args.port}/")
    print(f"SSE stream at http://{args.host}:{args.port}/events")
    web.run_app(app, host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
