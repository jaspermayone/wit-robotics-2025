# Battlebot Repo Review

> **Reviewer**: Claude Opus 4.6 (AI assistant, Anthropic)
> **Date**: 2026-04-13
> **Context**: Review conducted during scoping of the Foxglove integration. This is an outside-eyes pass on the firmware architecture to identify integration points and gaps.

## Architecture Summary

~3,400 lines of clean C targeting Pico 2 W. Modular design with vtable-based motor abstraction.

```
src/
  config/      — config.h (all pins, motor params, WiFi, battery settings)
  hardware/    — motor control, PWM, IMU, DHT11, SD card drivers
  networking/  — WiFi AP, web server, HTTP dashboard, SSE telemetry
  bluetooth/   — Bluepad32 platform callbacks, Xbox controller mapping
  tools/       — standalone motor_test utility
```

## Key Technical Details

### Motor Control Stack
- `motor_t` base interface with vtable pattern (polymorphic C)
- `motor_bi` — bidirectional ESC driver (throttle + reverse signals, "Brake Type 1.2")
- `motor_omni` — unidirectional ESC driver (weapon)
- `motor_controller` — high-level tank drive + weapon with failsafe, ramping, expo curves
- 50 Hz servo-style PWM (1100-1940 us pulse width)

### Sensor Suite
- **IMU**: 9-DOF via UART0 @ 9600 baud, custom 11-byte packet protocol
- **DHT11**: Single-wire temp/humidity, polled every 3s
- **Battery**: ADC0 with 5.7x voltage divider ratio, 3S LiPo thresholds defined
- **Fans**: Tachometer RPM sensing on GP5/GP7

### Networking
- WiFi AP mode (SSID: "Monster Book of Monsters", channel 11)
- lwIP TCP server on port 80
- SSE broadcast via btstack timer at 200ms interval
- Max 4 SSE clients with backpressure detection and stall timeout (3s)
- Web dashboard served as static HTML asset

### Safety
- 2-second failsafe timeout (no commands → stop)
- Controller disconnect → immediate e-stop + LED off + rescan
- 7% deadband to prevent drift
- Low battery cutoff configurable (currently disabled)
- Web-based e-stop toggle

### What's NOT There (Foxglove opportunities)
- No data logging/recording (SD card driver exists but isn't hooked up)
- No post-match replay capability
- No structured message schemas (just CSV over SSE)
- No event/annotation system for match timeline
- Dashboard is functional but basic HTML — no timeline scrubbing, no overlay

## Code Quality Notes

- Well-organized, consistent style
- Good separation of concerns (hardware drivers vs. control logic vs. networking)
- Proper error handling on TCP operations
- Backpressure management on SSE is thoughtful (stall timeout, client eviction)
- Config is centralized in `config.h` — easy to tune
- Cross-platform build scripts (Windows, macOS, Linux)
