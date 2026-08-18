#include "telemetry.hpp"

#include <cmath>

namespace battlebot {

namespace {

constexpr double kPi = 3.14159265358979323846;

double toRadians(double degrees) { return degrees * kPi / 180.0; }

}  // namespace

Quaternion eulerDegreesToQuaternion(double roll_deg, double pitch_deg, double yaw_deg) {
    const double roll = toRadians(roll_deg);
    const double pitch = toRadians(pitch_deg);
    const double yaw = toRadians(yaw_deg);

    const double cr = std::cos(roll * 0.5);
    const double sr = std::sin(roll * 0.5);
    const double cp = std::cos(pitch * 0.5);
    const double sp = std::sin(pitch * 0.5);
    const double cy = std::cos(yaw * 0.5);
    const double sy = std::sin(yaw * 0.5);

    Quaternion q;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    q.w = cr * cp * cy + sr * sp * sy;
    return q;
}

}  // namespace battlebot
