"""
Generate a fake MCAP file simulating a ~2 minute battlebot match.

Uses the same schemas as the live bridge so layouts work seamlessly with
either source:
  /tf              foxglove.FrameTransform (STANDARD)
  /motors          battlebot.Motors (custom)
  /imu             battlebot.Imu (custom composite: orientation + angular
                   velocity + linear acceleration)
  /battery         battlebot.Battery (custom)
  /thermal         battlebot.Thermal (custom)
  /state           battlebot.State (custom)
  /events/match    battlebot.Event (custom)

Usage:
    uv run python generate_fake_mcap.py
    uv run python generate_fake_mcap.py --output /path/to/file.mcap
    uv run python generate_fake_mcap.py --seed 42     # reproducible

Default output: ../mcap/fake-match.mcap
"""

import argparse
import json
import math
import random
from dataclasses import dataclass, replace
from pathlib import Path

from foxglove.messages import FrameTransform, Quaternion, Vector3
from mcap.writer import Writer

# ---------------------------------------------------------------------------
# Custom JSON schemas (match bridge.py exactly)
# ---------------------------------------------------------------------------

SCHEMA_MOTORS = json.dumps(
    {
        "type": "object",
        "description": "Motor command values for the tank drive + weapon.",
        "properties": {
            "left": {"type": "number"},
            "right": {"type": "number"},
            "weapon": {"type": "number"},
        },
    }
).encode()

SCHEMA_IMU = json.dumps(
    {
        "type": "object",
        "description": "IMU data matching sensor_msgs/Imu layout.",
        "properties": {
            "orientation": {
                "type": "object",
                "properties": {
                    "x": {"type": "number"},
                    "y": {"type": "number"},
                    "z": {"type": "number"},
                    "w": {"type": "number"},
                },
            },
            "angular_velocity": {
                "type": "object",
                "properties": {
                    "x": {"type": "number"},
                    "y": {"type": "number"},
                    "z": {"type": "number"},
                },
            },
            "linear_acceleration": {
                "type": "object",
                "properties": {
                    "x": {"type": "number"},
                    "y": {"type": "number"},
                    "z": {"type": "number"},
                },
            },
        },
    }
).encode()

SCHEMA_BATTERY = json.dumps(
    {
        "type": "object",
        "description": "Battery voltage and estimated charge percentage.",
        "properties": {
            "voltage": {"type": "number", "description": "Pack voltage (V)"},
            "cell_avg": {"type": "number", "description": "Per-cell average (V)"},
            "percent": {"type": "number", "description": "Estimated charge %"},
        },
    }
).encode()

SCHEMA_THERMAL = json.dumps(
    {
        "type": "object",
        "properties": {
            "temperature_c": {"type": "number"},
            "humidity": {"type": "number"},
        },
    }
).encode()

SCHEMA_STATE = json.dumps(
    {
        "type": "object",
        "properties": {
            "controller": {"type": "string", "enum": ["ACTIVE", "INIT", "STOPPED"]},
            "failsafe": {"type": "boolean"},
            "command_age_ms": {"type": "number"},
        },
    }
).encode()

SCHEMA_EVENT = json.dumps(
    {
        "type": "object",
        "description": "Discrete match event marker. Custom schema (not foxglove.Event).",
        "properties": {
            "name": {"type": "string"},
            "detail": {"type": "string"},
        },
    }
).encode()


# ---------------------------------------------------------------------------
# Match simulation
# ---------------------------------------------------------------------------

MATCH_DURATION_SEC = 120  # 2-minute match
SAMPLE_RATE_HZ = 5
DT = 1.0 / SAMPLE_RATE_HZ


@dataclass
class MatchPhase:
    name: str
    start_sec: float
    end_sec: float


PHASES = [
    MatchPhase("init", 0, 3),
    MatchPhase("cautious_approach", 3, 15),
    MatchPhase("weapon_spinup", 15, 20),
    MatchPhase("first_engagement", 20, 35),
    MatchPhase("hit_received", 35, 38),
    MatchPhase("recovery", 38, 45),
    MatchPhase("aggressive_driving", 45, 70),
    MatchPhase("second_hit", 70, 73),
    MatchPhase("weapon_down_recovery", 73, 80),
    MatchPhase("final_push", 80, 110),
    MatchPhase("match_end", 110, 120),
]


def get_phase(t: float) -> MatchPhase:
    for phase in PHASES:
        if phase.start_sec <= t < phase.end_sec:
            return phase
    return PHASES[-1]


@dataclass
class SimState:
    left_speed: float = 0.0
    right_speed: float = 0.0
    weapon_speed: float = 0.0
    roll: float = 0.0
    pitch: float = 0.0
    yaw: float = 0.0
    accel_x: float = 0.0
    accel_y: float = 0.0
    accel_z: float = 9.8
    gyro_x: float = 0.0
    gyro_y: float = 0.0
    gyro_z: float = 0.0
    battery_v: float = 12.4
    temp_c: float = 28.0
    humidity: float = 48.0
    controller_state: str = "INIT"
    failsafe: bool = False
    command_age_ms: float = 0.0


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def noise(amplitude: float) -> float:
    return (random.random() - 0.5) * 2 * amplitude


def smooth_step(t: float) -> float:
    return t * t * (3 - 2 * t)


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


def simulate_step(t: float, prev: SimState) -> SimState:
    phase = get_phase(t)
    s = replace(prev)
    phase_t = (t - phase.start_sec) / (phase.end_sec - phase.start_sec)

    s.controller_state = "ACTIVE"
    s.failsafe = False
    s.command_age_ms = 10 + random.random() * 30

    if phase.name == "init":
        s.controller_state = "INIT" if phase_t < 0.3 else "ACTIVE"
        s.left_speed = 0
        s.right_speed = 0
        s.weapon_speed = 0
        s.roll = noise(0.5)
        s.pitch = noise(0.5)
        s.yaw = prev.yaw + noise(0.2)

    elif phase.name == "cautious_approach":
        s.left_speed = lerp(0, 35, smooth_step(phase_t)) + noise(3)
        s.right_speed = lerp(0, 30, smooth_step(phase_t)) + noise(3)
        s.weapon_speed = 0
        s.yaw = prev.yaw + (s.left_speed - s.right_speed) * 0.05 + noise(0.3)
        s.pitch = noise(1.5)
        s.roll = noise(1)

    elif phase.name == "weapon_spinup":
        s.left_speed = 25 + noise(5)
        s.right_speed = 22 + noise(5)
        s.weapon_speed = lerp(0, 85, smooth_step(phase_t)) + noise(2)
        s.yaw = prev.yaw + noise(0.5)
        s.roll = prev.roll + s.weapon_speed * 0.02 + noise(0.5)
        s.pitch = noise(2)

    elif phase.name == "first_engagement":
        drive_pattern = math.sin(t * 0.8) * 60 + 30
        turn_pattern = math.sin(t * 1.5) * 25
        s.left_speed = clamp(drive_pattern + turn_pattern + noise(8), -100, 100)
        s.right_speed = clamp(drive_pattern - turn_pattern + noise(8), -100, 100)
        s.weapon_speed = 90 + noise(5)
        s.yaw = prev.yaw + turn_pattern * 0.1 + noise(1)
        s.roll = math.sin(t * 2) * 5 + noise(2)
        s.pitch = math.cos(t * 1.7) * 4 + noise(2)

    elif phase.name == "hit_received":
        impact_decay = math.exp(-(t - phase.start_sec) * 2)
        s.left_speed = prev.left_speed * 0.3 + noise(30) * impact_decay
        s.right_speed = prev.right_speed * 0.3 + noise(30) * impact_decay
        s.weapon_speed = lerp(prev.weapon_speed, 40, phase_t) + noise(10)
        s.roll = 35 * impact_decay * math.sin(t * 15) + noise(5)
        s.pitch = 25 * impact_decay * math.cos(t * 12) + noise(5)
        s.yaw = prev.yaw + 45 * impact_decay + noise(3)
        s.accel_x = 8 * impact_decay + noise(2)
        s.accel_y = -6 * impact_decay + noise(2)
        s.accel_z = 9.8 + 12 * impact_decay + noise(1)
        s.gyro_x = 200 * impact_decay * math.sin(t * 10)
        s.gyro_y = 150 * impact_decay * math.cos(t * 8)
        s.gyro_z = 100 * impact_decay

    elif phase.name == "recovery":
        s.left_speed = lerp(prev.left_speed, 15, 0.15) + noise(5)
        s.right_speed = lerp(prev.right_speed, 12, 0.15) + noise(5)
        s.weapon_speed = lerp(prev.weapon_speed, 75, smooth_step(phase_t))
        s.roll = lerp(prev.roll, 0, 0.2) + noise(1)
        s.pitch = lerp(prev.pitch, 0, 0.2) + noise(1)
        s.yaw = prev.yaw + noise(0.5)

    elif phase.name == "aggressive_driving":
        pattern = math.sin(t * 0.5)
        aggression = 0.7 + 0.3 * math.sin(t * 0.2)
        s.left_speed = clamp(
            pattern * 80 * aggression + math.cos(t * 1.2) * 30 + noise(10), -100, 100
        )
        s.right_speed = clamp(
            pattern * 75 * aggression - math.cos(t * 1.2) * 30 + noise(10), -100, 100
        )
        s.weapon_speed = 85 + noise(5)
        s.yaw = prev.yaw + (s.left_speed - s.right_speed) * 0.05 + noise(1)
        s.roll = math.sin(t * 3) * 8 + s.weapon_speed * 0.015 + noise(2)
        s.pitch = math.cos(t * 2.5) * 6 + noise(2)

    elif phase.name == "second_hit":
        impact_decay = math.exp(-(t - phase.start_sec) * 1.5)
        s.left_speed = noise(40) * impact_decay
        s.right_speed = noise(40) * impact_decay
        s.weapon_speed = lerp(prev.weapon_speed, 0, smooth_step(phase_t))
        s.roll = -40 * impact_decay * math.sin(t * 12) + noise(8)
        s.pitch = 30 * impact_decay * math.cos(t * 10) + noise(6)
        s.yaw = prev.yaw - 60 * impact_decay + noise(5)
        s.accel_x = -10 * impact_decay + noise(3)
        s.accel_y = 7 * impact_decay + noise(3)
        s.accel_z = 9.8 + 15 * impact_decay + noise(2)
        s.gyro_x = -250 * impact_decay * math.sin(t * 8)
        s.gyro_y = 180 * impact_decay * math.cos(t * 7)
        s.gyro_z = -120 * impact_decay

    elif phase.name == "weapon_down_recovery":
        s.left_speed = lerp(prev.left_speed, -20, 0.1) + noise(8)
        s.right_speed = lerp(prev.right_speed, -25, 0.1) + noise(8)
        s.weapon_speed = clamp(lerp(prev.weapon_speed, 0, 0.3) + noise(2), 0, 100)
        s.roll = lerp(prev.roll, 0, 0.15) + noise(1.5)
        s.pitch = lerp(prev.pitch, 0, 0.15) + noise(1.5)
        s.yaw = prev.yaw + noise(1)

    elif phase.name == "final_push":
        desperation = math.sin(t * 0.7)
        s.left_speed = clamp(desperation * 90 + noise(12), -100, 100)
        s.right_speed = clamp(desperation * 85 + noise(12), -100, 100)
        s.weapon_speed = clamp(lerp(prev.weapon_speed, 50, 0.05) + noise(5), 0, 100)
        s.yaw = prev.yaw + (s.left_speed - s.right_speed) * 0.04 + noise(1.5)
        s.roll = math.sin(t * 2) * 6 + noise(3)
        s.pitch = math.cos(t * 1.8) * 5 + noise(3)

    elif phase.name == "match_end":
        s.left_speed = lerp(prev.left_speed, 0, 0.15)
        s.right_speed = lerp(prev.right_speed, 0, 0.15)
        s.weapon_speed = lerp(prev.weapon_speed, 0, 0.1)
        s.controller_state = "STOPPED" if phase_t > 0.8 else "ACTIVE"
        s.roll = lerp(prev.roll, 0, 0.2) + noise(0.3)
        s.pitch = lerp(prev.pitch, 0, 0.2) + noise(0.3)
        s.yaw = prev.yaw + noise(0.1)

    # Default accel/gyro when not set by impact phases
    if phase.name not in ("hit_received", "second_hit"):
        total_speed = (abs(s.left_speed) + abs(s.right_speed)) / 200
        s.accel_x = total_speed * 2 + noise(0.3)
        s.accel_y = ((s.left_speed - s.right_speed) / 100) * 3 + noise(0.3)
        s.accel_z = 9.8 + noise(0.15)
        s.gyro_x = (s.roll - prev.roll) / DT + noise(2)
        s.gyro_y = (s.pitch - prev.pitch) / DT + noise(2)
        s.gyro_z = (s.yaw - prev.yaw) / DT + noise(3)

    motor_load = (abs(s.left_speed) + abs(s.right_speed) + s.weapon_speed * 1.5) / 350
    base_drain = t * (0.008 / MATCH_DURATION_SEC)
    load_sag = motor_load * 0.4
    s.battery_v = clamp(12.4 - base_drain - load_sag + noise(0.02), 10.0, 12.6)

    s.temp_c = lerp(prev.temp_c, 28 + t * 0.08 + motor_load * 15, 0.02) + noise(0.1)
    s.humidity = clamp(lerp(prev.humidity, 45 - t * 0.05, 0.01) + noise(0.2), 30, 55)

    return s


# ---------------------------------------------------------------------------
# Match events (battlebot.Event, NOT foxglove.Event)
# ---------------------------------------------------------------------------


@dataclass
class MatchEvent:
    time_sec: float
    name: str
    detail: str


EVENTS = [
    MatchEvent(0, "MATCH_START", "Fight! 2-minute match begins"),
    MatchEvent(3, "CONTROLLER_CONNECTED", "Xbox controller paired via Bluetooth"),
    MatchEvent(15, "WEAPON_SPINUP", "Weapon motor engaging — spinner at 85%"),
    MatchEvent(20, "FIRST_CONTACT", "Engaging opponent"),
    MatchEvent(35, "HIT_RECEIVED", "Major impact — IMU spike detected"),
    MatchEvent(38, "RECOVERY", "Regaining control after hit"),
    MatchEvent(70, "HIT_RECEIVED", "Second major impact — weapon motor stalled"),
    MatchEvent(
        73, "WEAPON_DOWN", "Weapon motor not responding — switching to ram strategy"
    ),
    MatchEvent(95, "LOW_BATTERY_WARNING", "Battery at 11.0V — 3.67V/cell"),
    MatchEvent(110, "MATCH_END", "Time! Match complete — judges' decision"),
]


def sec_to_ns(sec: float) -> int:
    return int(sec * 1e9)


def main() -> None:
    default_output = Path(__file__).parent.parent / "mcap" / "fake-match.mcap"

    parser = argparse.ArgumentParser(description="Generate a fake battlebot match MCAP")
    parser.add_argument("--output", "-o", type=str, default=str(default_output))
    parser.add_argument(
        "--seed", type=int, default=None, help="Random seed for reproducible output"
    )
    args = parser.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with open(output_path, "wb") as f:
        writer = Writer(f)
        writer.start(profile="foxglove", library="battlebot-sim")

        # /tf: standard foxglove.FrameTransform (protobuf)
        # Get the schema definition directly from the foxglove-sdk so it matches
        # exactly what the bridge produces.
        ft_schema = FrameTransform.get_schema()
        tf_schema_id = writer.register_schema(
            name=ft_schema.name,
            encoding=ft_schema.encoding,
            data=ft_schema.data,
        )
        tf_channel_id = writer.register_channel(
            schema_id=tf_schema_id,
            topic="/tf",
            message_encoding="protobuf",
        )

        # Custom JSON schemas
        motors_schema_id = writer.register_schema(
            name="battlebot.Motors", encoding="jsonschema", data=SCHEMA_MOTORS
        )
        motors_channel_id = writer.register_channel(
            schema_id=motors_schema_id, topic="/motors", message_encoding="json"
        )

        imu_schema_id = writer.register_schema(
            name="battlebot.Imu", encoding="jsonschema", data=SCHEMA_IMU
        )
        imu_channel_id = writer.register_channel(
            schema_id=imu_schema_id, topic="/imu", message_encoding="json"
        )

        battery_schema_id = writer.register_schema(
            name="battlebot.Battery", encoding="jsonschema", data=SCHEMA_BATTERY
        )
        battery_channel_id = writer.register_channel(
            schema_id=battery_schema_id, topic="/battery", message_encoding="json"
        )

        thermal_schema_id = writer.register_schema(
            name="battlebot.Thermal", encoding="jsonschema", data=SCHEMA_THERMAL
        )
        thermal_channel_id = writer.register_channel(
            schema_id=thermal_schema_id, topic="/thermal", message_encoding="json"
        )

        state_schema_id = writer.register_schema(
            name="battlebot.State", encoding="jsonschema", data=SCHEMA_STATE
        )
        state_channel_id = writer.register_channel(
            schema_id=state_schema_id, topic="/state", message_encoding="json"
        )

        event_schema_id = writer.register_schema(
            name="battlebot.Event", encoding="jsonschema", data=SCHEMA_EVENT
        )
        event_channel_id = writer.register_channel(
            schema_id=event_schema_id, topic="/events/match", message_encoding="json"
        )

        # Simulate
        state = SimState()
        total_samples = MATCH_DURATION_SEC * SAMPLE_RATE_HZ
        next_event_idx = 0

        for i in range(total_samples + 1):
            t = i * DT
            log_time_ns = sec_to_ns(t)
            state = simulate_step(t, state)

            qx, qy, qz, qw = euler_deg_to_quaternion(state.roll, state.pitch, state.yaw)

            # Events at their scheduled times
            while next_event_idx < len(EVENTS) and EVENTS[next_event_idx].time_sec <= t:
                evt = EVENTS[next_event_idx]
                writer.add_message(
                    channel_id=event_channel_id,
                    log_time=log_time_ns,
                    publish_time=log_time_ns,
                    sequence=i,
                    data=json.dumps({"name": evt.name, "detail": evt.detail}).encode(),
                )
                next_event_idx += 1

            # /tf — standard FrameTransform protobuf message
            tf_msg = FrameTransform(
                parent_frame_id="world",
                child_frame_id="battlebot",
                translation=Vector3(x=0.0, y=0.0, z=0.0),
                rotation=Quaternion(x=qx, y=qy, z=qz, w=qw),
            )
            writer.add_message(
                channel_id=tf_channel_id,
                log_time=log_time_ns,
                publish_time=log_time_ns,
                sequence=i,
                data=tf_msg.encode(),
            )

            # /motors — 5 Hz
            writer.add_message(
                channel_id=motors_channel_id,
                log_time=log_time_ns,
                publish_time=log_time_ns,
                sequence=i,
                data=json.dumps(
                    {
                        "left": round(state.left_speed, 1),
                        "right": round(state.right_speed, 1),
                        "weapon": round(state.weapon_speed, 1),
                    }
                ).encode(),
            )

            # /imu — 5 Hz, full composite
            writer.add_message(
                channel_id=imu_channel_id,
                log_time=log_time_ns,
                publish_time=log_time_ns,
                sequence=i,
                data=json.dumps(
                    {
                        "orientation": {"x": qx, "y": qy, "z": qz, "w": qw},
                        "angular_velocity": {
                            "x": round(math.radians(state.gyro_x), 4),
                            "y": round(math.radians(state.gyro_y), 4),
                            "z": round(math.radians(state.gyro_z), 4),
                        },
                        "linear_acceleration": {
                            "x": round(state.accel_x, 3),
                            "y": round(state.accel_y, 3),
                            "z": round(state.accel_z, 3),
                        },
                    }
                ).encode(),
            )

            # /battery — 1 Hz
            if i % SAMPLE_RATE_HZ == 0:
                writer.add_message(
                    channel_id=battery_channel_id,
                    log_time=log_time_ns,
                    publish_time=log_time_ns,
                    sequence=i,
                    data=json.dumps(
                        {
                            "voltage": round(state.battery_v, 2),
                            "cell_avg": round(state.battery_v / 3, 2),
                            "percent": round(
                                clamp(
                                    ((state.battery_v - 10.0) / (12.6 - 10.0)) * 100,
                                    0,
                                    100,
                                )
                            ),
                        }
                    ).encode(),
                )

            # /thermal — ~0.33 Hz
            if i % (SAMPLE_RATE_HZ * 3) == 0:
                writer.add_message(
                    channel_id=thermal_channel_id,
                    log_time=log_time_ns,
                    publish_time=log_time_ns,
                    sequence=i,
                    data=json.dumps(
                        {
                            "temperature_c": round(state.temp_c, 1),
                            "humidity": round(state.humidity),
                        }
                    ).encode(),
                )

            # /state — 1 Hz
            if i % SAMPLE_RATE_HZ == 0:
                writer.add_message(
                    channel_id=state_channel_id,
                    log_time=log_time_ns,
                    publish_time=log_time_ns,
                    sequence=i,
                    data=json.dumps(
                        {
                            "controller": state.controller_state,
                            "failsafe": state.failsafe,
                            "command_age_ms": round(state.command_age_ms),
                        }
                    ).encode(),
                )

        writer.add_metadata(
            name="robot",
            data={
                "name": "Monster Book of Monsters",
                "mcu": "Raspberry Pi Pico 2 W",
                "match_type": "simulated",
                "match_duration": f"{MATCH_DURATION_SEC}s",
            },
        )

        writer.finish()

    print(f"Wrote {output_path}")
    print(
        f"  {total_samples + 1} time steps, {MATCH_DURATION_SEC}s match, {SAMPLE_RATE_HZ}Hz"
    )
    print(f"  {len(EVENTS)} events")
    print("  7 channels: /tf, /motors, /imu, /battery, /thermal, /state, /events/match")


if __name__ == "__main__":
    main()
