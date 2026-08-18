#pragma once

// The orientation maths shared with the firmware.
//
// The bot serves Foxglove directly, so there is no SSE parsing here any more.
// `src/networking/foxglove_ws.c:fg_euler_to_quaternion()` in the firmware is
// the same computation in C. Keep the two in step.

namespace battlebot {

/// A quaternion in the same field order Foxglove uses.
struct Quaternion {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
};

/// Converts Euler angles in degrees to a quaternion, in the XYZ order that
/// Foxglove's `.@rpy` message path function expects.
Quaternion eulerDegreesToQuaternion(double roll_deg, double pitch_deg, double yaw_deg);

}  // namespace battlebot
