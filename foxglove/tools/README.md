# Foxglove Tools

Python tools for bridging the battlebot's SSE telemetry to Foxglove, and for generating demo data.

## Prerequisites

- Python 3.10+
- [uv](https://docs.astral.sh/uv/) — install with `curl -LsSf https://astral.sh/uv/install.sh | sh`

## Scripts

| Script | Purpose |
|---|---|
| `bridge.py` | SSE-to-Foxglove bridge — consumes bot telemetry, serves Foxglove WebSocket, records MCAP |
| `fake_sse_server.py` | Fake bot SSE server for demos without real hardware |
| `generate_fake_mcap.py` | Generate a 2-minute simulated match as an MCAP file |

## Quick Start

### With fake data (demo)

```bash
# Terminal 1 — fake bot
uv run python fake_sse_server.py

# Terminal 2 — bridge
uv run python bridge.py --url http://localhost:7002/events
```

Open Foxglove, connect to `ws://localhost:7001`, import `../layouts/battlebot-dashboard.json`.

### With real bot

1. Connect laptop to the bot's WiFi AP ("Monster Book of Monsters")
2. Run the bridge:

```bash
uv run python bridge.py
```

The default URL is `http://192.168.4.1/events` (the bot's AP address).

### Generate a fake match MCAP

```bash
uv run python generate_fake_mcap.py
# writes to ../mcap/fake-match.mcap

# Reproducible output:
uv run python generate_fake_mcap.py --seed 42
```

## Bridge Options

```
bridge.py [--url URL] [--ws-port PORT] [--no-ws] [--no-mcap]
          [--output-dir DIR] [--bot-api URL]

  --url          SSE endpoint (default: http://192.168.4.1/events)
  --ws-port      Foxglove WebSocket port (default: 7001)
  --ws-host      Bind address (default: 0.0.0.0)
  --no-ws        Disable WebSocket server (MCAP recording only)
  --no-mcap      Disable MCAP recording (live viewing only)
  --output-dir   MCAP output directory (default: ../mcap/)
  --bot-api      Bot HTTP API base URL for bench commands
                 (default: http://192.168.4.1)
```

## Ports

- **7001** — Foxglove WebSocket (bridge output)
- **7002** — fake SSE server

## SSE Format

The bot broadcasts CSV over SSE at 200ms intervals:

```
data: STATE,failsafe,left,right,weapon,age_ms,roll,pitch,yaw,test_active,test_motor,test_power,test_remaining_ms,temp_c,humidity
```

See `src/networking/web_server.c:generate_sse_frame()` in the firmware for the source of truth.

## Bench Commands (Publish Panel)

The Foxglove layout includes Publish buttons that send commands via the bridge to the bot's HTTP API. These are **bench-only**:

| Topic | Action | Maps to |
|---|---|---|
| `/cmd/estop` | Toggle active/stopped state | `POST /api/state/toggle` |
| `/cmd/test` | Run a motor test | `POST /api/test/start?motor=&power=&duration=` |
| `/cmd/test_stop` | Stop motor test | `POST /api/test/stop` |

**Do not rely on these as a real e-stop.** Always have a physical e-stop ready.
