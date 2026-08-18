#pragma once

// Channel schemas shared by the bridge and the match generator.
//
// Both tools publish the same schemas, so a Foxglove layout works with either
// source. Standard Foxglove schemas are used where they fit. The `battlebot.*`
// schemas cover the rest.

#include <foxglove/schema.hpp>

namespace battlebot::schemas {

/// Motor command values for the tank drive and the weapon.
foxglove::Schema motors();

/// IMU data laid out like sensor_msgs/Imu.
foxglove::Schema imu();

/// Battery voltage and estimated charge.
foxglove::Schema battery();

/// Internal temperature and humidity from the DHT11 sensor.
foxglove::Schema thermal();

/// Controller state and safety flags.
foxglove::Schema state();

/// A discrete match event marker.
foxglove::Schema event();

}  // namespace battlebot::schemas
