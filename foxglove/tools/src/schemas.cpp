#include "schemas.hpp"

#include <string_view>

namespace battlebot::schemas {

namespace {

/// Wraps a static JSON Schema document as a Foxglove schema. The data stays
/// valid for the life of the program because the literal has static storage.
foxglove::Schema make(std::string_view name, std::string_view document) {
    foxglove::Schema schema;
    schema.name = std::string(name);
    schema.encoding = "jsonschema";
    schema.data = reinterpret_cast<const std::byte*>(document.data());
    schema.data_len = document.size();
    return schema;
}

constexpr std::string_view kMotors = R"JSON({
  "type": "object",
  "description": "Motor command values for the tank drive + weapon.",
  "properties": {
    "left": {"type": "number", "description": "Left drive motor (-100..100)"},
    "right": {"type": "number", "description": "Right drive motor (-100..100)"},
    "weapon": {"type": "number", "description": "Weapon spinner motor (0..100)"}
  }
})JSON";

constexpr std::string_view kImu = R"JSON({
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
        "w": {"type": "number"}
      }
    },
    "angular_velocity": {
      "type": "object",
      "description": "Angular velocity in rad/s (foxglove.Vector3 shape)",
      "properties": {
        "x": {"type": "number"},
        "y": {"type": "number"},
        "z": {"type": "number"}
      }
    },
    "linear_acceleration": {
      "type": "object",
      "description": "Linear acceleration in m/s^2 (foxglove.Vector3 shape)",
      "properties": {
        "x": {"type": "number"},
        "y": {"type": "number"},
        "z": {"type": "number"}
      }
    }
  }
})JSON";

constexpr std::string_view kBattery = R"JSON({
  "type": "object",
  "description": "Battery voltage and estimated charge percentage.",
  "properties": {
    "voltage": {"type": "number", "description": "Pack voltage (V)"},
    "cell_avg": {"type": "number", "description": "Per-cell average (V)"},
    "percent": {"type": "number", "description": "Estimated charge %"}
  }
})JSON";

constexpr std::string_view kThermal = R"JSON({
  "type": "object",
  "description": "Internal temperature and humidity from the DHT11 sensor.",
  "properties": {
    "temperature_c": {"type": "number", "description": "Internal temperature (C)"},
    "humidity": {"type": "number", "description": "Humidity (%)"}
  }
})JSON";

constexpr std::string_view kState = R"JSON({
  "type": "object",
  "description": "Controller state and safety flags. Drives Indicator panels.",
  "properties": {
    "controller": {"type": "string", "enum": ["ACTIVE", "INIT", "STOPPED"]},
    "failsafe": {"type": "boolean"},
    "command_age_ms": {"type": "number", "description": "Time since last controller command (ms)"}
  }
})JSON";

constexpr std::string_view kEvent = R"JSON({
  "type": "object",
  "description": "Discrete match event marker. Custom schema (not foxglove.Event).",
  "properties": {
    "name": {"type": "string"},
    "detail": {"type": "string"}
  }
})JSON";

}  // namespace

foxglove::Schema motors() { return make("battlebot.Motors", kMotors); }
foxglove::Schema imu() { return make("battlebot.Imu", kImu); }
foxglove::Schema battery() { return make("battlebot.Battery", kBattery); }
foxglove::Schema thermal() { return make("battlebot.Thermal", kThermal); }
foxglove::Schema state() { return make("battlebot.State", kState); }
foxglove::Schema event() { return make("battlebot.Event", kEvent); }

}  // namespace battlebot::schemas
