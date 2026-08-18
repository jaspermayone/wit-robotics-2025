# Foxglove Tools

A C++ tool that generates demo data for Foxglove.

**There is no bridge here any more.** The bot serves the Foxglove WebSocket
protocol itself, from `src/networking/foxglove_ws.c` in the firmware. To see
live telemetry, connect Foxglove to `ws://<bot-ip>:8765`. See `../README.md`.

This project exists so you can work on layouts and panels without the hardware.

## Prerequisites

- A C++17 compiler (Apple Clang, GCC 9+, or MSVC 2019+)
- CMake 3.20 or later
- An internet connection for the first build

The build downloads the [Foxglove C++ SDK](https://docs.foxglove.dev/docs/getting-started/cpp)
with CMake `FetchContent` and checks it against a known SHA256. There is no
other dependency.

## Build

```bash
cmake -S . -B build
cmake --build build
```

This is a standalone project. It is **not** part of the firmware build in the
repository root, which cross-compiles for the Pico 2 W.

| Binary | Purpose |
|---|---|
| `build/generate_fake_mcap` | Generates a 2 minute simulated match as an MCAP file. |

## Quick Start

```bash
./build/generate_fake_mcap
# writes to ../mcap/fake-match.mcap

# Repeatable output:
./build/generate_fake_mcap --seed 42
```

Open the file in Foxglove and import `../layouts/battlebot-dashboard.json`.

The generator writes to the repository's `mcap/` directory by default, whatever
directory you run it from.

## Generator Options

```
generate_fake_mcap [-o PATH] [--seed N]

  -o, --output   Output file (default: ../mcap/fake-match.mcap)
      --seed     Random seed, for a repeatable match
```

## Source Layout

| File | Purpose |
|---|---|
| `src/generate_fake_mcap.cpp` | The match generator |
| `src/schemas.cpp` | The `battlebot.*` JSON schemas |
| `src/telemetry.cpp` | The Euler-to-quaternion maths |
| `src/json.cpp` | A small JSON writer |
| `cmake/FoxgloveSdk.cmake` | Downloads and verifies the Foxglove SDK |

## Keeping the Schemas in Step

Two places define the `battlebot.*` schemas, and they must match:

| Place | Used by |
|---|---|
| `src/schemas.cpp` | This generator |
| `src/networking/foxglove_ws.c` (`kSchema*` literals) | The bot |

`src/telemetry.cpp:eulerDegreesToQuaternion()` and
`src/networking/foxglove_ws.c:fg_euler_to_quaternion()` are the same
computation, one in C++ and one in C. Change both together.

The generator publishes two extra channels that the bot does not: `/battery`
and `/events/match`. That is on purpose. See the roadmap in `../README.md`.
