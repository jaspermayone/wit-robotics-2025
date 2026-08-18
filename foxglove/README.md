# Foxglove Integration for Monster Book of Monsters

Foxglove tooling for the Pico 2 W battlebot — live telemetry, post-match replay, and bench-testing controls.

## What's Here

- **`tools/`** — Python scripts (SSE bridge, fake SSE server, MCAP generator) managed with [uv](https://docs.astral.sh/uv/)
- **`layouts/`** — Foxglove layouts ready to import
- **`mcap/`** — Landing zone for MCAP recordings (gitignored contents)
- **`repo-review.md`** — Technical review of the battlebot firmware

## Goals

The battlebot already has a WiFi AP serving an SSE telemetry stream at 5 Hz. These tools layer Foxglove on top to unlock:

1. **Live visualization** — real-time Foxglove panels during testing (no firmware changes needed)
2. **Post-match replay** — MCAP recording for timeline-scrubbing after a match
3. **Event markers** — annotate key moments (failsafe, e-stop, weapon events) on the timeline
4. **Bench commands** — trigger e-stop and motor tests from the Foxglove UI (testing only)

## Quick Start

```bash
# Terminal 1 — fake bot (for demos without the real hardware)
cd tools
uv run python fake_sse_server.py

# Terminal 2 — bridge
uv run python bridge.py --url http://localhost:7002/events
```

Then open [Foxglove](https://app.foxglove.dev), connect to `ws://localhost:7001`, and import `layouts/battlebot-dashboard.json`.

For the real bot:

1. Connect your laptop to the bot's WiFi AP ("Monster Book of Monsters")
2. `uv run python bridge.py` (defaults to `http://192.168.4.1/events`)

See `tools/README.md` for full options.

## Architecture

```mermaid
graph LR
    A[Pico 2 W<br/>WiFi AP] -->|SSE @ 5Hz| B[bridge.py]
    B -->|Foxglove WS| C[Foxglove App<br/>Live Panels]
    B -->|MCAP| D[mcap/ landing zone]
    D -->|Post-session| C
    style B fill:#f9f,stroke:#333
    style D fill:#ff9,stroke:#333
```

## Demo Data

A reference recording is committed at **`mcap/fake-match.mcap`** — open it in Foxglove with `layouts/battlebot-dashboard.json` to see every panel populated (including accel/gyro/battery, which the live SSE bridge can't produce today).

`tools/generate_fake_mcap.py` simulates a 2-minute match with realistic phase progression: init → weapon spinup → engagement → **big hit** → recovery → aggressive combat → **second hit** → final push → end.

```bash
cd tools
uv run python generate_fake_mcap.py                 # fresh run with random data
uv run python generate_fake_mcap.py --seed 42       # reproducible (committed version uses this seed)
```

### Why do accel/gyro show up in `fake-match.mcap` but NOT in live bridge recordings?

The bot's SSE stream (`web_server.c:generate_sse_frame`) only includes `roll`, `pitch`, `yaw`, `temp_c`, `humidity`, and motor speeds — not accel, gyro, or battery voltage. The bridge emits zeros for those fields because it has nothing to populate them with. The generator bypasses SSE entirely and simulates a full match including accel/gyro data, which is why its MCAP output is richer. Adding those fields to the SSE format is on the roadmap below.

## Schemas

The bridge and generator publish the same schemas so layouts work with either source. Standard Foxglove schemas are used where they fit; custom `battlebot.*` schemas cover the rest.

| Topic | Schema | Notes |
|---|---|---|
| `/tf` | `foxglove.FrameTransform` | **Standard.** Drives the 3D panel. Quaternion computed from IMU roll/pitch/yaw. |
| `/motors` | `battlebot.Motors` | Custom. `{left, right, weapon}` — no standard for motor commands. |
| `/imu` | `battlebot.Imu` | Custom composite matching `sensor_msgs/Imu`: orientation (Quaternion), angular_velocity (Vector3), linear_acceleration (Vector3). |
| `/battery` | `battlebot.Battery` | Custom. Not populated by the bridge (SSE doesn't include voltage yet). |
| `/thermal` | `battlebot.Thermal` | Custom. `{temperature_c, humidity}`. |
| `/state` | `battlebot.State` | Custom. `{controller, failsafe, command_age_ms}` — drives the Indicator panels. |
| `/events/match` | `battlebot.Event` | Custom. Generator-only for now. |

**Plotting Euler angles**: The layout uses Foxglove's built-in `.@rpy` function to extract roll/pitch/yaw from the quaternion — e.g. `/imu.orientation.@rpy.yaw.@degrees`.

## Roadmap

### Done
- **SSE-to-Foxglove bridge** — Python script using foxglove-sdk multi-sink (MCAP + WebSocket simultaneously)
- **Standard FrameTransform for 3D** — live 3D view of bot orientation
- **Foxglove layout** — motor plots, IMU (with `.@rpy` Euler extraction), battery, thermal, state indicators, Publish buttons
- **Bench commands** — `/cmd/estop`, `/cmd/test`, `/cmd/test_stop` topics forwarded to bot HTTP API

### Next
- **Extend SSE with accel, gyro, battery voltage** — the bot reads these but doesn't broadcast them. Adding them to `web_server.c:generate_sse_frame()` would make the accel/gyro/battery plots light up during live sessions (currently populated only by the generator).
- **MCAP-to-SD firmware work** — hook the existing `sd_storage.c` driver into the SSE telemetry pipeline so the bot records its own MCAP files on-device. The SD card driver is already implemented but `sd_card_init()` is never called.
- **Event markers** — emit `battlebot.Event` messages on a `/events/match` channel for key match moments. `tools/generate_fake_mcap.py` demonstrates the pattern.

## Safety

**The Publish panel buttons in the layout are for BENCH TESTING ONLY.** Do not rely on them as a real e-stop. Always wear safety goggles and have a physical e-stop ready when working with high-amp systems. The WiFi → WebSocket → Python → HTTP chain adds latency and has multiple failure points — it is not safety-critical.

## Key Links

- [Foxglove SDK](https://github.com/foxglove/foxglove-sdk)
- [Foxglove docs](https://docs.foxglove.dev)
- [MCAP spec](https://mcap.dev)
