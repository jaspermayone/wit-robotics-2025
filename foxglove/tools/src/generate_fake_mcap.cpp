// Generates a fake MCAP file that simulates a 2 minute battlebot match.
//
// The channels and schemas are the same as the live bridge, so one Foxglove
// layout works with either source:
//   /tf             foxglove.FrameTransform (STANDARD)
//   /motors         battlebot.Motors (custom)
//   /imu            battlebot.Imu (custom composite: orientation + angular
//                   velocity + linear acceleration)
//   /battery        battlebot.Battery (custom)
//   /thermal        battlebot.Thermal (custom)
//   /state          battlebot.State (custom)
//   /events/match   battlebot.Event (custom)
//
// The recording carries more channels than a live bridge recording. The bot's
// SSE stream has no accelerometer, gyroscope or battery data, so the bridge
// cannot publish /battery, and it sends zeros for the accel and gyro fields.

#include <foxglove/channel.hpp>
#include <foxglove/context.hpp>
#include <foxglove/error.hpp>
#include <foxglove/foxglove.hpp>
#include <foxglove/mcap.hpp>
#include <foxglove/messages.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "schemas.hpp"
#include "telemetry.hpp"

namespace json = battlebot::json;
namespace schemas = battlebot::schemas;

namespace {

// The MCAP directory that ships with the repository. Baked in at configure
// time so the tool writes where the layouts and the sample recording live,
// whatever directory it is run from.
#ifndef BATTLEBOT_DEFAULT_MCAP_DIR
#define BATTLEBOT_DEFAULT_MCAP_DIR "."
#endif

constexpr int kMatchDurationSec = 120;  // 2 minute match
constexpr int kSampleRateHz = 5;
constexpr double kDt = 1.0 / kSampleRateHz;

// -------------------------------------------------------------------------
// Match simulation
// -------------------------------------------------------------------------

struct MatchPhase {
    const char* name;
    double start_sec;
    double end_sec;
};

const MatchPhase kPhases[] = {
    {"init", 0, 3},
    {"cautious_approach", 3, 15},
    {"weapon_spinup", 15, 20},
    {"first_engagement", 20, 35},
    {"hit_received", 35, 38},
    {"recovery", 38, 45},
    {"aggressive_driving", 45, 70},
    {"second_hit", 70, 73},
    {"weapon_down_recovery", 73, 80},
    {"final_push", 80, 110},
    {"match_end", 110, 120},
};

constexpr size_t kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);

const MatchPhase& getPhase(double t) {
    for (const MatchPhase& phase : kPhases) {
        if (t >= phase.start_sec && t < phase.end_sec) {
            return phase;
        }
    }
    return kPhases[kPhaseCount - 1];
}

struct SimState {
    double left_speed = 0.0;
    double right_speed = 0.0;
    double weapon_speed = 0.0;
    double roll = 0.0;
    double pitch = 0.0;
    double yaw = 0.0;
    double accel_x = 0.0;
    double accel_y = 0.0;
    double accel_z = 9.8;
    double gyro_x = 0.0;
    double gyro_y = 0.0;
    double gyro_z = 0.0;
    double battery_v = 12.4;
    double temp_c = 28.0;
    double humidity = 48.0;
    std::string controller_state = "INIT";
    bool failsafe = false;
    double command_age_ms = 0.0;
};

struct MatchEvent {
    double time_sec;
    const char* name;
    const char* detail;
};

const MatchEvent kEvents[] = {
    {0, "MATCH_START", "Fight! 2-minute match begins"},
    {3, "CONTROLLER_CONNECTED", "Xbox controller paired via Bluetooth"},
    {15, "WEAPON_SPINUP", "Weapon motor engaging - spinner at 85%"},
    {20, "FIRST_CONTACT", "Engaging opponent"},
    {35, "HIT_RECEIVED", "Major impact - IMU spike detected"},
    {38, "RECOVERY", "Regaining control after hit"},
    {70, "HIT_RECEIVED", "Second major impact - weapon motor stalled"},
    {73, "WEAPON_DOWN", "Weapon motor not responding - switching to ram strategy"},
    {95, "LOW_BATTERY_WARNING", "Battery at 11.0V - 3.67V/cell"},
    {110, "MATCH_END", "Time! Match complete - judges' decision"},
};

constexpr size_t kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);

// The one random source for the whole run. A fixed seed gives the same match
// every time.
std::mt19937 g_random(std::random_device{}());
std::uniform_real_distribution<double> g_unit(0.0, 1.0);

double random01() { return g_unit(g_random); }

double clamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

double noise(double amplitude) { return (random01() - 0.5) * 2 * amplitude; }

double smoothStep(double t) { return t * t * (3 - 2 * t); }

/// Rounds to a number of decimal places, so the JSON stays readable.
double roundTo(double value, int digits) {
    const double scale = std::pow(10.0, digits);
    return std::round(value * scale) / scale;
}

constexpr double kPi = 3.14159265358979323846;

double toRadians(double degrees) { return degrees * kPi / 180.0; }

/// Advances the simulation by one sample.
SimState simulateStep(double t, const SimState& prev) {
    const MatchPhase& phase = getPhase(t);
    SimState s = prev;
    const double phase_t = (t - phase.start_sec) / (phase.end_sec - phase.start_sec);
    const std::string name = phase.name;

    s.controller_state = "ACTIVE";
    s.failsafe = false;
    s.command_age_ms = 10 + random01() * 30;

    if (name == "init") {
        s.controller_state = phase_t < 0.3 ? "INIT" : "ACTIVE";
        s.left_speed = 0;
        s.right_speed = 0;
        s.weapon_speed = 0;
        s.roll = noise(0.5);
        s.pitch = noise(0.5);
        s.yaw = prev.yaw + noise(0.2);

    } else if (name == "cautious_approach") {
        s.left_speed = lerp(0, 35, smoothStep(phase_t)) + noise(3);
        s.right_speed = lerp(0, 30, smoothStep(phase_t)) + noise(3);
        s.weapon_speed = 0;
        s.yaw = prev.yaw + (s.left_speed - s.right_speed) * 0.05 + noise(0.3);
        s.pitch = noise(1.5);
        s.roll = noise(1);

    } else if (name == "weapon_spinup") {
        s.left_speed = 25 + noise(5);
        s.right_speed = 22 + noise(5);
        s.weapon_speed = lerp(0, 85, smoothStep(phase_t)) + noise(2);
        s.yaw = prev.yaw + noise(0.5);
        s.roll = prev.roll + s.weapon_speed * 0.02 + noise(0.5);
        s.pitch = noise(2);

    } else if (name == "first_engagement") {
        const double drive_pattern = std::sin(t * 0.8) * 60 + 30;
        const double turn_pattern = std::sin(t * 1.5) * 25;
        s.left_speed = clamp(drive_pattern + turn_pattern + noise(8), -100, 100);
        s.right_speed = clamp(drive_pattern - turn_pattern + noise(8), -100, 100);
        s.weapon_speed = 90 + noise(5);
        s.yaw = prev.yaw + turn_pattern * 0.1 + noise(1);
        s.roll = std::sin(t * 2) * 5 + noise(2);
        s.pitch = std::cos(t * 1.7) * 4 + noise(2);

    } else if (name == "hit_received") {
        const double impact_decay = std::exp(-(t - phase.start_sec) * 2);
        s.left_speed = prev.left_speed * 0.3 + noise(30) * impact_decay;
        s.right_speed = prev.right_speed * 0.3 + noise(30) * impact_decay;
        s.weapon_speed = lerp(prev.weapon_speed, 40, phase_t) + noise(10);
        s.roll = 35 * impact_decay * std::sin(t * 15) + noise(5);
        s.pitch = 25 * impact_decay * std::cos(t * 12) + noise(5);
        s.yaw = prev.yaw + 45 * impact_decay + noise(3);
        s.accel_x = 8 * impact_decay + noise(2);
        s.accel_y = -6 * impact_decay + noise(2);
        s.accel_z = 9.8 + 12 * impact_decay + noise(1);
        s.gyro_x = 200 * impact_decay * std::sin(t * 10);
        s.gyro_y = 150 * impact_decay * std::cos(t * 8);
        s.gyro_z = 100 * impact_decay;

    } else if (name == "recovery") {
        s.left_speed = lerp(prev.left_speed, 15, 0.15) + noise(5);
        s.right_speed = lerp(prev.right_speed, 12, 0.15) + noise(5);
        s.weapon_speed = lerp(prev.weapon_speed, 75, smoothStep(phase_t));
        s.roll = lerp(prev.roll, 0, 0.2) + noise(1);
        s.pitch = lerp(prev.pitch, 0, 0.2) + noise(1);
        s.yaw = prev.yaw + noise(0.5);

    } else if (name == "aggressive_driving") {
        const double pattern = std::sin(t * 0.5);
        const double aggression = 0.7 + 0.3 * std::sin(t * 0.2);
        s.left_speed =
            clamp(pattern * 80 * aggression + std::cos(t * 1.2) * 30 + noise(10), -100, 100);
        s.right_speed =
            clamp(pattern * 75 * aggression - std::cos(t * 1.2) * 30 + noise(10), -100, 100);
        s.weapon_speed = 85 + noise(5);
        s.yaw = prev.yaw + (s.left_speed - s.right_speed) * 0.05 + noise(1);
        s.roll = std::sin(t * 3) * 8 + s.weapon_speed * 0.015 + noise(2);
        s.pitch = std::cos(t * 2.5) * 6 + noise(2);

    } else if (name == "second_hit") {
        const double impact_decay = std::exp(-(t - phase.start_sec) * 1.5);
        s.left_speed = noise(40) * impact_decay;
        s.right_speed = noise(40) * impact_decay;
        s.weapon_speed = lerp(prev.weapon_speed, 0, smoothStep(phase_t));
        s.roll = -40 * impact_decay * std::sin(t * 12) + noise(8);
        s.pitch = 30 * impact_decay * std::cos(t * 10) + noise(6);
        s.yaw = prev.yaw - 60 * impact_decay + noise(5);
        s.accel_x = -10 * impact_decay + noise(3);
        s.accel_y = 7 * impact_decay + noise(3);
        s.accel_z = 9.8 + 15 * impact_decay + noise(2);
        s.gyro_x = -250 * impact_decay * std::sin(t * 8);
        s.gyro_y = 180 * impact_decay * std::cos(t * 7);
        s.gyro_z = -120 * impact_decay;

    } else if (name == "weapon_down_recovery") {
        s.left_speed = lerp(prev.left_speed, -20, 0.1) + noise(8);
        s.right_speed = lerp(prev.right_speed, -25, 0.1) + noise(8);
        s.weapon_speed = clamp(lerp(prev.weapon_speed, 0, 0.3) + noise(2), 0, 100);
        s.roll = lerp(prev.roll, 0, 0.15) + noise(1.5);
        s.pitch = lerp(prev.pitch, 0, 0.15) + noise(1.5);
        s.yaw = prev.yaw + noise(1);

    } else if (name == "final_push") {
        const double desperation = std::sin(t * 0.7);
        s.left_speed = clamp(desperation * 90 + noise(12), -100, 100);
        s.right_speed = clamp(desperation * 85 + noise(12), -100, 100);
        s.weapon_speed = clamp(lerp(prev.weapon_speed, 50, 0.05) + noise(5), 0, 100);
        s.yaw = prev.yaw + (s.left_speed - s.right_speed) * 0.04 + noise(1.5);
        s.roll = std::sin(t * 2) * 6 + noise(3);
        s.pitch = std::cos(t * 1.8) * 5 + noise(3);

    } else if (name == "match_end") {
        s.left_speed = lerp(prev.left_speed, 0, 0.15);
        s.right_speed = lerp(prev.right_speed, 0, 0.15);
        s.weapon_speed = lerp(prev.weapon_speed, 0, 0.1);
        s.controller_state = phase_t > 0.8 ? "STOPPED" : "ACTIVE";
        s.roll = lerp(prev.roll, 0, 0.2) + noise(0.3);
        s.pitch = lerp(prev.pitch, 0, 0.2) + noise(0.3);
        s.yaw = prev.yaw + noise(0.1);
    }

    // Default accel and gyro, for every phase that is not an impact.
    if (name != "hit_received" && name != "second_hit") {
        const double total_speed = (std::fabs(s.left_speed) + std::fabs(s.right_speed)) / 200;
        s.accel_x = total_speed * 2 + noise(0.3);
        s.accel_y = ((s.left_speed - s.right_speed) / 100) * 3 + noise(0.3);
        s.accel_z = 9.8 + noise(0.15);
        s.gyro_x = (s.roll - prev.roll) / kDt + noise(2);
        s.gyro_y = (s.pitch - prev.pitch) / kDt + noise(2);
        s.gyro_z = (s.yaw - prev.yaw) / kDt + noise(3);
    }

    const double motor_load =
        (std::fabs(s.left_speed) + std::fabs(s.right_speed) + s.weapon_speed * 1.5) / 350;
    const double base_drain = t * (0.008 / kMatchDurationSec);
    const double load_sag = motor_load * 0.4;
    s.battery_v = clamp(12.4 - base_drain - load_sag + noise(0.02), 10.0, 12.6);

    s.temp_c = lerp(prev.temp_c, 28 + t * 0.08 + motor_load * 15, 0.02) + noise(0.1);
    s.humidity = clamp(lerp(prev.humidity, 45 - t * 0.05, 0.01) + noise(0.2), 30, 55);

    return s;
}

// -------------------------------------------------------------------------
// Output
// -------------------------------------------------------------------------

uint64_t secToNs(double seconds) { return static_cast<uint64_t>(seconds * 1e9); }

/// Takes the value out of a Foxglove result, or reports the error and exits.
template <typename T>
T unwrapOrExit(foxglove::FoxgloveResult<T>&& result, const char* what) {
    if (!result.has_value()) {
        std::fprintf(stderr, "[sim] Cannot create %s: %s\n", what,
                     foxglove::strerror(result.error()));
        std::exit(1);
    }
    return std::move(result.value());
}

void logJson(foxglove::RawChannel& channel, const std::string& document, uint64_t log_time) {
    channel.log(reinterpret_cast<const std::byte*>(document.data()), document.size(), log_time);
}

struct Options {
    std::string output =
        (std::filesystem::path(BATTLEBOT_DEFAULT_MCAP_DIR) / "fake-match.mcap").string();
    bool have_seed = false;
    unsigned seed = 0;
};

void printUsage() {
    std::printf(
        "Generate a fake battlebot match MCAP\n"
        "\n"
        "Usage: generate_fake_mcap [options]\n"
        "\n"
        "  -o, --output PATH  Output file (default: %s)\n"
        "      --seed N       Random seed, for a repeatable match\n"
        "  -h, --help         Show this message\n",
        (std::filesystem::path(BATTLEBOT_DEFAULT_MCAP_DIR) / "fake-match.mcap").string().c_str());
}

bool parseOptions(int argc, char** argv, Options& options, bool& want_help) {
    want_help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "-h" || flag == "--help") {
            want_help = true;
            return true;
        }
        if (flag == "-o" || flag == "--output") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[sim] %s needs a value\n", flag.c_str());
                return false;
            }
            options.output = argv[++i];
        } else if (flag == "--seed") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[sim] --seed needs a value\n");
                return false;
            }
            options.seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
            options.have_seed = true;
        } else {
            std::fprintf(stderr, "[sim] Unknown option: %s\n", flag.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace battlebot;

    Options options;
    bool want_help = false;
    if (!parseOptions(argc, argv, options, want_help)) {
        return 2;
    }
    if (want_help) {
        printUsage();
        return 0;
    }
    if (options.have_seed) {
        g_random.seed(options.seed);
    }

    const std::filesystem::path output_path(options.output);
    if (output_path.has_parent_path()) {
        std::error_code created;
        std::filesystem::create_directories(output_path.parent_path(), created);
        if (created) {
            std::fprintf(stderr, "[sim] Cannot create %s: %s\n",
                         output_path.parent_path().string().c_str(), created.message().c_str());
            return 1;
        }
    }

    foxglove::setLogLevel(foxglove::LogLevel::Warn);

    // A private context, so only this writer receives the simulated messages.
    const foxglove::Context context = foxglove::Context::create();

    foxglove::McapWriterOptions writer_options;
    writer_options.context = context;
    writer_options.path = options.output;
    writer_options.profile = "foxglove";
    writer_options.truncate = true;

    auto opened = foxglove::McapWriter::create(writer_options);
    if (!opened.has_value()) {
        std::fprintf(stderr, "[sim] Cannot open %s: %s\n", options.output.c_str(),
                     foxglove::strerror(opened.error()));
        return 1;
    }
    foxglove::McapWriter writer = std::move(opened.value());

    foxglove::messages::FrameTransformChannel tf =
        unwrapOrExit(foxglove::messages::FrameTransformChannel::create("/tf", context), "/tf channel");
    foxglove::RawChannel motors = unwrapOrExit(
        foxglove::RawChannel::create("/motors", "json", schemas::motors(), context),
        "/motors channel");
    foxglove::RawChannel imu = unwrapOrExit(
        foxglove::RawChannel::create("/imu", "json", schemas::imu(), context), "/imu channel");
    foxglove::RawChannel battery = unwrapOrExit(
        foxglove::RawChannel::create("/battery", "json", schemas::battery(), context),
        "/battery channel");
    foxglove::RawChannel thermal = unwrapOrExit(
        foxglove::RawChannel::create("/thermal", "json", schemas::thermal(), context),
        "/thermal channel");
    foxglove::RawChannel state = unwrapOrExit(
        foxglove::RawChannel::create("/state", "json", schemas::state(), context),
        "/state channel");
    foxglove::RawChannel events = unwrapOrExit(
        foxglove::RawChannel::create("/events/match", "json", schemas::event(), context),
        "/events/match channel");

    SimState sim;
    const int total_samples = kMatchDurationSec * kSampleRateHz;
    size_t next_event = 0;

    for (int i = 0; i <= total_samples; ++i) {
        const double t = i * kDt;
        const uint64_t log_time = secToNs(t);
        sim = simulateStep(t, sim);

        const Quaternion q = eulerDegreesToQuaternion(sim.roll, sim.pitch, sim.yaw);

        // Events, at their scheduled times.
        while (next_event < kEventCount && kEvents[next_event].time_sec <= t) {
            json::Writer writer_json;
            writer_json.key("name").value(std::string(kEvents[next_event].name))
                .key("detail").value(std::string(kEvents[next_event].detail));
            logJson(events, writer_json.finish(), log_time);
            ++next_event;
        }

        // /tf - standard FrameTransform for the 3D panel.
        foxglove::messages::FrameTransform transform;
        transform.parent_frame_id = "world";
        transform.child_frame_id = "battlebot";
        transform.translation = foxglove::messages::Vector3{0.0, 0.0, 0.0};
        transform.rotation = foxglove::messages::Quaternion{q.x, q.y, q.z, q.w};
        tf.log(transform, log_time);

        // /motors - 5 Hz
        {
            json::Writer w;
            w.key("left").value(roundTo(sim.left_speed, 1))
                .key("right").value(roundTo(sim.right_speed, 1))
                .key("weapon").value(roundTo(sim.weapon_speed, 1));
            logJson(motors, w.finish(), log_time);
        }

        // /imu - 5 Hz, full composite
        {
            json::Writer w;
            w.beginObject("orientation")
                .key("x").value(q.x)
                .key("y").value(q.y)
                .key("z").value(q.z)
                .key("w").value(q.w)
                .endObject();
            w.beginObject("angular_velocity")
                .key("x").value(roundTo(toRadians(sim.gyro_x), 4))
                .key("y").value(roundTo(toRadians(sim.gyro_y), 4))
                .key("z").value(roundTo(toRadians(sim.gyro_z), 4))
                .endObject();
            w.beginObject("linear_acceleration")
                .key("x").value(roundTo(sim.accel_x, 3))
                .key("y").value(roundTo(sim.accel_y, 3))
                .key("z").value(roundTo(sim.accel_z, 3))
                .endObject();
            logJson(imu, w.finish(), log_time);
        }

        // /battery - 1 Hz
        if (i % kSampleRateHz == 0) {
            const double percent = clamp(((sim.battery_v - 10.0) / (12.6 - 10.0)) * 100, 0, 100);
            json::Writer w;
            w.key("voltage").value(roundTo(sim.battery_v, 2))
                .key("cell_avg").value(roundTo(sim.battery_v / 3, 2))
                .key("percent").value(std::round(percent));
            logJson(battery, w.finish(), log_time);
        }

        // /thermal - about 0.33 Hz
        if (i % (kSampleRateHz * 3) == 0) {
            json::Writer w;
            w.key("temperature_c").value(roundTo(sim.temp_c, 1))
                .key("humidity").value(std::round(sim.humidity));
            logJson(thermal, w.finish(), log_time);
        }

        // /state - 1 Hz
        if (i % kSampleRateHz == 0) {
            json::Writer w;
            w.key("controller").value(sim.controller_state)
                .key("failsafe").value(sim.failsafe)
                .key("command_age_ms").value(std::round(sim.command_age_ms));
            logJson(state, w.finish(), log_time);
        }
    }

    const std::vector<std::pair<std::string, std::string>> metadata = {
        {"name", "Monster Book of Monsters"},
        {"mcu", "Raspberry Pi Pico 2 W"},
        {"match_type", "simulated"},
        {"match_duration", std::to_string(kMatchDurationSec) + "s"},
    };
    writer.writeMetadata("robot", metadata.begin(), metadata.end());
    writer.close();

    std::printf("Wrote %s\n", options.output.c_str());
    std::printf("  %d time steps, %ds match, %dHz\n", total_samples + 1, kMatchDurationSec,
                kSampleRateHz);
    std::printf("  %zu events\n", kEventCount);
    std::printf("  7 channels: /tf, /motors, /imu, /battery, /thermal, /state, /events/match\n");
    return 0;
}
