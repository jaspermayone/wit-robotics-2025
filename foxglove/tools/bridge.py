"""
SSE-to-Foxglove bridge for the Monster Book of Monsters battlebot.

Consumes the bot's SSE telemetry stream and publishes to both:
  - Foxglove WebSocket server (live visualization)
  - MCAP file on disk (post-match replay)

Both sinks are active simultaneously via the foxglove-sdk's multi-sink
context. Each channel.log() call writes to both.

Schemas:
  /tf        - foxglove.FrameTransform (STANDARD) — drives the 3D panel
  /motors    - battlebot.Motors (custom, no standard for motor commands)
  /imu       - battlebot.Imu (custom composite: orientation + angular_velocity
                              + linear_acceleration, mirrors sensor_msgs/Imu)
  /thermal   - battlebot.Thermal (custom, no standard)
  /state     - battlebot.State (custom, for Indicator panels)

Bench command topics (inbound from Foxglove Publish panel):
  /cmd/estop     - toggle bot active/stopped
  /cmd/test      - run a motor test
  /cmd/test_stop - stop motor test

Usage:
    # Against real bot (connected to its WiFi AP):
    uv run python bridge.py

    # Against fake SSE server (for development):
    uv run python bridge.py --url http://localhost:7002/events

    # MCAP-only (no WebSocket server):
    uv run python bridge.py --no-ws

    # WebSocket-only (no MCAP recording):
    uv run python bridge.py --no-mcap

Then open Foxglove and connect to ws://localhost:7001
"""

import argparse
import asyncio
import json
import math
import signal
import time
import urllib.request
import urllib.error
from dataclasses import dataclass
from pathlib import Path

import aiohttp
import foxglove
from foxglove import Channel, Schema
from foxglove.channels import FrameTransformChannel
from foxglove.messages import FrameTransform, Quaternion, Vector3
from foxglove.websocket import Capability, Client, ClientChannel, ServerListener

# ---------------------------------------------------------------------------
# SSE field definitions (must match web_server.c:generate_sse_frame)
# ---------------------------------------------------------------------------

SSE_FIELDS = [
    "state",  # ACTIVE | INIT | STOPPED
    "failsafe",  # 0 | 1
    "left",  # motor speed -100..100
    "right",  # motor speed -100..100
    "weapon",  # motor speed 0..100
    "age_ms",  # ms since last controller command
    "roll",  # degrees
    "pitch",  # degrees
    "yaw",  # degrees
    "test_active",  # 0 | 1
    "test_motor",  # motor name or "none"
    "test_power",  # 0..100
    "test_remaining_ms",  # ms
    "temp_c",  # celsius
    "humidity",  # percent
]


@dataclass
class TelemetryFrame:
    state: str
    failsafe: bool
    left: float
    right: float
    weapon: float
    age_ms: int
    roll: float
    pitch: float
    yaw: float
    test_active: bool
    test_motor: str
    test_power: int
    test_remaining_ms: int
    temp_c: int
    humidity: int

    @classmethod
    def from_csv(cls, csv_line: str) -> "TelemetryFrame":
        parts = csv_line.strip().split(",")
        if len(parts) != len(SSE_FIELDS):
            raise ValueError(
                f"Expected {len(SSE_FIELDS)} fields, got {len(parts)}: {csv_line!r}"
            )
        return cls(
            state=parts[0],
            failsafe=parts[1] == "1",
            left=float(parts[2]),
            right=float(parts[3]),
            weapon=float(parts[4]),
            age_ms=int(parts[5]),
            roll=float(parts[6]),
            pitch=float(parts[7]),
            yaw=float(parts[8]),
            test_active=parts[9] == "1",
            test_motor=parts[10],
            test_power=int(parts[11]),
            test_remaining_ms=int(parts[12]),
            temp_c=int(parts[13]),
            humidity=int(parts[14]),
        )


# ---------------------------------------------------------------------------
# Quaternion helper (XYZ Euler order, matches Foxglove's .@rpy function)
# ---------------------------------------------------------------------------


def euler_deg_to_quaternion(
    roll_deg: float, pitch_deg: float, yaw_deg: float
) -> tuple[float, float, float, float]:
    roll = math.radians(roll_deg)
    pitch = math.radians(pitch_deg)
    yaw = math.radians(yaw_deg)

    cr, sr = math.cos(roll * 0.5), math.sin(roll * 0.5)
    cp, sp = math.cos(pitch * 0.5), math.sin(pitch * 0.5)
    cy, sy = math.cos(yaw * 0.5), math.sin(yaw * 0.5)

    x = sr * cp * cy - cr * sp * sy
    y = cr * sp * cy + sr * cp * sy
    z = cr * cp * sy - sr * sp * cy
    w = cr * cp * cy + sr * sp * sy
    return (x, y, z, w)


# ---------------------------------------------------------------------------
# Custom JSON schemas (for things not covered by standard Foxglove schemas)
# ---------------------------------------------------------------------------

MOTOR_SCHEMA = Schema(
    name="battlebot.Motors",
    encoding="jsonschema",
    data=json.dumps(
        {
            "type": "object",
            "description": "Motor command values for the tank drive + weapon.",
            "properties": {
                "left": {
                    "type": "number",
                    "description": "Left drive motor (-100..100)",
                },
                "right": {
                    "type": "number",
                    "description": "Right drive motor (-100..100)",
                },
                "weapon": {
                    "type": "number",
                    "description": "Weapon spinner motor (0..100)",
                },
            },
        }
    ).encode(),
)

# Mirrors sensor_msgs/Imu structure using Foxglove standard Quaternion + Vector3
# as nested field shapes. The .@rpy message path function works on the nested
# orientation object via field inference (x, y, z, w).
IMU_SCHEMA = Schema(
    name="battlebot.Imu",
    encoding="jsonschema",
    data=json.dumps(
        {
            "type": "object",
            "description": "IMU data matching sensor_msgs/Imu layout (orientation + angular velocity + linear acceleration).",
            "properties": {
                "orientation": {
                    "type": "object",
                    "description": "Orientation as quaternion (foxglove.Quaternion shape)",
                    "properties": {
                        "x": {"type": "number"},
                        "y": {"type": "number"},
                        "z": {"type": "number"},
                        "w": {"type": "number"},
                    },
                },
                "angular_velocity": {
                    "type": "object",
                    "description": "Angular velocity in rad/s (foxglove.Vector3 shape)",
                    "properties": {
                        "x": {"type": "number"},
                        "y": {"type": "number"},
                        "z": {"type": "number"},
                    },
                },
                "linear_acceleration": {
                    "type": "object",
                    "description": "Linear acceleration in m/s² (foxglove.Vector3 shape)",
                    "properties": {
                        "x": {"type": "number"},
                        "y": {"type": "number"},
                        "z": {"type": "number"},
                    },
                },
            },
        }
    ).encode(),
)

THERMAL_SCHEMA = Schema(
    name="battlebot.Thermal",
    encoding="jsonschema",
    data=json.dumps(
        {
            "type": "object",
            "description": "Internal temperature and humidity from the DHT11 sensor.",
            "properties": {
                "temperature_c": {
                    "type": "number",
                    "description": "Internal temperature (°C)",
                },
                "humidity": {"type": "number", "description": "Humidity (%)"},
            },
        }
    ).encode(),
)

STATE_SCHEMA = Schema(
    name="battlebot.State",
    encoding="jsonschema",
    data=json.dumps(
        {
            "type": "object",
            "description": "Controller state and safety flags. Drives Indicator panels.",
            "properties": {
                "controller": {
                    "type": "string",
                    "enum": ["ACTIVE", "INIT", "STOPPED"],
                },
                "failsafe": {"type": "boolean"},
                "command_age_ms": {
                    "type": "number",
                    "description": "Time since last controller command (ms)",
                },
            },
        }
    ).encode(),
)


# ---------------------------------------------------------------------------
# Bench commands — Foxglove Publish panel → HTTP to bot
#
# WARNING: This is a BENCH FEATURE for testing only. Do NOT rely on this as
# a real e-stop. Always wear safety goggles and have a physical e-stop ready
# when working with high-amp systems. The WiFi → WebSocket → Python → HTTP
# chain adds latency and has multiple failure points.
# ---------------------------------------------------------------------------

BOT_API_BASE = "http://192.168.4.1"


def send_bot_command(api_base: str, path: str) -> None:
    url = f"{api_base}{path}"
    try:
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=2) as resp:
            body = resp.read().decode()
            print(f"[cmd] {path} → {resp.status} {body.strip()}")
    except urllib.error.URLError as e:
        print(f"[cmd] {path} → ERROR: {e}")
    except TimeoutError:
        print(f"[cmd] {path} → TIMEOUT")


class BotCommandListener(ServerListener):
    """
    Listens for messages published from Foxglove (Publish panel) and
    forwards them as HTTP commands to the bot.
    """

    def __init__(self, api_base: str) -> None:
        self.api_base = api_base
        self._client_channels: dict[int, str] = {}

    def on_client_advertise(self, client: Client, channel: ClientChannel) -> None:
        self._client_channels[channel.id] = channel.topic
        print(f"[cmd] Client advertised {channel.topic}")

    def on_client_unadvertise(self, client: Client, client_channel_id: int) -> None:
        topic = self._client_channels.pop(client_channel_id, None)
        if topic:
            print(f"[cmd] Client unadvertised {topic}")

    def on_message_data(
        self, client: Client, client_channel_id: int, data: bytes
    ) -> None:
        topic = self._client_channels.get(client_channel_id)
        if topic is None:
            return

        if topic == "/cmd/estop":
            print("[cmd] E-STOP TOGGLE (bench mode)")
            send_bot_command(self.api_base, "/api/state/toggle")

        elif topic == "/cmd/test":
            try:
                msg = json.loads(data)
                motor = msg.get("motor", "left")
                power = int(msg.get("power", 20))
                duration = float(msg.get("duration", 2.0))
                path = (
                    f"/api/test/start?motor={motor}&power={power}&duration={duration}"
                )
                print(f"[cmd] Motor test: {motor} @ {power}% for {duration}s")
                send_bot_command(self.api_base, path)
            except (json.JSONDecodeError, ValueError) as e:
                print(f"[cmd] Bad /cmd/test payload: {e}")

        elif topic == "/cmd/test_stop":
            print("[cmd] Motor test stop")
            send_bot_command(self.api_base, "/api/test/stop")


# ---------------------------------------------------------------------------
# Channel setup
# ---------------------------------------------------------------------------


@dataclass
class Channels:
    tf: FrameTransformChannel  # standard Foxglove schema
    motors: Channel  # custom battlebot.Motors
    imu: Channel  # custom battlebot.Imu (composite)
    thermal: Channel  # custom battlebot.Thermal
    state: Channel  # custom battlebot.State


def create_channels() -> Channels:
    return Channels(
        tf=FrameTransformChannel(topic="/tf"),
        motors=Channel(topic="/motors", message_encoding="json", schema=MOTOR_SCHEMA),
        imu=Channel(topic="/imu", message_encoding="json", schema=IMU_SCHEMA),
        thermal=Channel(
            topic="/thermal", message_encoding="json", schema=THERMAL_SCHEMA
        ),
        state=Channel(topic="/state", message_encoding="json", schema=STATE_SCHEMA),
    )


# ---------------------------------------------------------------------------
# Publish a telemetry frame to all channels
# ---------------------------------------------------------------------------

_sample_count = 0
THERMAL_EVERY = 15  # ~0.33 Hz at 5 Hz input (matches DHT11 poll rate)
STATE_EVERY = 5  # 1 Hz at 5 Hz input


def publish_frame(channels: Channels, frame: TelemetryFrame) -> None:
    global _sample_count
    _sample_count += 1

    # Orientation (and /tf for 3D panel). Bridge has no accel/gyro — emit zeros.
    qx, qy, qz, qw = euler_deg_to_quaternion(frame.roll, frame.pitch, frame.yaw)

    # /tf — standard FrameTransform for the 3D panel (typed channel)
    channels.tf.log(
        FrameTransform(
            parent_frame_id="world",
            child_frame_id="battlebot",
            translation=Vector3(x=0.0, y=0.0, z=0.0),
            rotation=Quaternion(x=qx, y=qy, z=qz, w=qw),
        )
    )

    # /imu — composite message. Plots extract Euler via .@rpy.
    channels.imu.log(
        json.dumps(
            {
                "orientation": {"x": qx, "y": qy, "z": qz, "w": qw},
                "angular_velocity": {"x": 0.0, "y": 0.0, "z": 0.0},
                "linear_acceleration": {"x": 0.0, "y": 0.0, "z": 0.0},
            }
        ).encode()
    )

    # /motors — 5 Hz
    channels.motors.log(
        json.dumps(
            {
                "left": frame.left,
                "right": frame.right,
                "weapon": frame.weapon,
            }
        ).encode()
    )

    # /thermal — ~0.33 Hz
    if _sample_count % THERMAL_EVERY == 0:
        channels.thermal.log(
            json.dumps(
                {
                    "temperature_c": frame.temp_c,
                    "humidity": frame.humidity,
                }
            ).encode()
        )

    # /state — 1 Hz
    if _sample_count % STATE_EVERY == 0:
        channels.state.log(
            json.dumps(
                {
                    "controller": frame.state,
                    "failsafe": frame.failsafe,
                    "command_age_ms": frame.age_ms,
                }
            ).encode()
        )


# ---------------------------------------------------------------------------
# SSE consumer
# ---------------------------------------------------------------------------


async def consume_sse(
    url: str,
    channels: Channels,
    reconnect_delay: float = 2.0,
) -> None:
    while True:
        try:
            print(f"[bridge] Connecting to {url}")
            async with aiohttp.ClientSession() as session:
                async with session.get(
                    url, timeout=aiohttp.ClientTimeout(total=None)
                ) as resp:
                    if resp.status != 200:
                        print(f"[bridge] HTTP {resp.status} from {url}")
                        await asyncio.sleep(reconnect_delay)
                        continue

                    print("[bridge] Connected — streaming telemetry")
                    buffer = ""
                    async for chunk in resp.content.iter_any():
                        buffer += chunk.decode(errors="replace")
                        while "\n\n" in buffer:
                            event, buffer = buffer.split("\n\n", 1)
                            for line in event.strip().splitlines():
                                if line.startswith("data: "):
                                    csv_data = line[6:]
                                    try:
                                        frame = TelemetryFrame.from_csv(csv_data)
                                        publish_frame(channels, frame)
                                    except ValueError as e:
                                        print(f"[bridge] Parse error: {e}")

        except aiohttp.ClientError as e:
            print(f"[bridge] Connection error: {e}")
        except asyncio.CancelledError:
            print("[bridge] Shutting down")
            return

        print(f"[bridge] Reconnecting in {reconnect_delay}s...")
        await asyncio.sleep(reconnect_delay)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(
        description="SSE-to-Foxglove bridge for Monster Book of Monsters"
    )
    parser.add_argument(
        "--url",
        type=str,
        default="http://192.168.4.1/events",
        help="SSE endpoint URL (default: bot's WiFi AP)",
    )
    parser.add_argument("--ws-port", type=int, default=7001, help="Foxglove WS port")
    parser.add_argument("--ws-host", type=str, default="0.0.0.0")
    parser.add_argument("--no-ws", action="store_true", help="Disable WebSocket server")
    parser.add_argument("--no-mcap", action="store_true", help="Disable MCAP recording")
    parser.add_argument(
        "--output-dir",
        type=str,
        default=str(Path(__file__).parent.parent / "mcap"),
        help="Directory for MCAP files (default: ../mcap/)",
    )
    parser.add_argument(
        "--bot-api",
        type=str,
        default=BOT_API_BASE,
        help="Bot HTTP API base URL for bench commands",
    )
    args = parser.parse_args()

    if args.no_ws and args.no_mcap:
        print("[bridge] Error: both --no-ws and --no-mcap specified, nothing to do")
        return

    # Multi-sink: both sinks attach to the default foxglove Context, so every
    # channel.log() writes to both the MCAP file and the WebSocket clients.
    mcap_writer = None
    ws_server = None

    if not args.no_mcap:
        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        mcap_path = output_dir / f"battlebot_{timestamp}.mcap"
        mcap_writer = foxglove.open_mcap(str(mcap_path))
        print(f"[bridge] Recording to {mcap_path}")

    if not args.no_ws:
        listener = BotCommandListener(api_base=args.bot_api)
        ws_server = foxglove.start_server(
            name="Monster Book of Monsters",
            host=args.ws_host,
            port=args.ws_port,
            capabilities=[Capability.ClientPublish],
            server_listener=listener,
        )
        print(f"[bridge] Foxglove WS at ws://{args.ws_host}:{args.ws_port}")
        print(f"[bridge] Bench commands enabled → {args.bot_api}")

    channels = create_channels()

    loop = asyncio.new_event_loop()

    def shutdown() -> None:
        for task in asyncio.all_tasks(loop):
            task.cancel()

    loop.add_signal_handler(signal.SIGINT, shutdown)
    loop.add_signal_handler(signal.SIGTERM, shutdown)

    try:
        loop.run_until_complete(consume_sse(args.url, channels))
    except KeyboardInterrupt:
        pass
    finally:
        if mcap_writer is not None:
            mcap_writer.close()
            print("[bridge] MCAP file finalized")
        if ws_server is not None:
            ws_server.stop()
            print("[bridge] WebSocket server stopped")
        loop.close()


if __name__ == "__main__":
    main()
