/**
 * @file config.h
 * @brief Central configuration — all hardware pins, motor settings, and tunables.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "secrets.h"  // Contains SECRET_WIFI_SSID and SECRET_WIFI_PASSWORD (gitignored)

// =============================================================================
// ROBOT IDENTITY
// =============================================================================

#define ROBOT_NAME  "Monster Book of Monsters"

// =============================================================================
// HARDWARE PIN ASSIGNMENTS
// =============================================================================

// IMU Pins
#define imu_uart    uart0
#define imu_tx_pin  12
#define imu_rx_pin  13


// ESC Motor Pins (PWM signal to ESC)
#define PIN_MOTOR_TRH_LEFT         0
#define PIN_MOTOR_REV_LEFT         1
#define PIN_MOTOR_TRH_RIGHT        2
#define PIN_MOTOR_REV_RIGHT        3
#define PIN_WEAPON                 6  // Not yet wired

// DHT11 Temperature/Humidity Sensor
#define PIN_DHT11              4

// Noctua NF-A4x10 5V PWM Fans (25kHz PWM)
#define PIN_FAN1_PWM           14
#define PIN_FAN1_TACH          5
#define PIN_FAN2_PWM           15
#define PIN_FAN2_TACH          7

// WS2812B LED Strip (PIO driven)
#define PIN_LED_STRIP          8
#define LED_STRIP_COUNT        75  // ~50 inches at 60 LEDs/m
#define LED_STRIP_MAX_BRIGHTNESS 128  // Cap at 50% to stay under 3A step-down limit

// Analog Inputs
#define PIN_BATTERY_ADC  26  // ADC0 - Battery voltage divider

// Status LED is handled by CYW43 (CYW43_WL_GPIO_LED_PIN)

// =============================================================================
// MOTOR SETTINGS
// =============================================================================

// ESC PWM Configuration (50Hz servo-style)
#define MOTOR_PWM_FREQ      50      // Hz

// Motor Behavior
#define MOTOR_MAX_SPEED     100     // Maximum speed percentage
#define MOTOR_DEADBAND      7       // Ignore inputs below this %

// Ramping (% per controller update, ~100Hz)
// Higher = faster response, lower = smoother
#define MOTOR_RAMP_UP       5       // Max speed increase per update
#define MOTOR_RAMP_DOWN     10      // Max speed decrease per update (brake faster than accelerate)
#define MOTOR_INTERPOLATION    0.3f    // Exponential smoothing factor for speed changes (0.0 = no smoothing, 1.0 = infinite smoothing)

// =============================================================================
// SAFETY SETTINGS
// =============================================================================

#define FAILSAFE_ENABLED    false
#define FAILSAFE_TIMEOUT_MS 2000    // Time without commands before triggering failsafe

/*
PWM pulse widths in microseconds

The ESC uses 50 Hz servo-style PWM (one pulse every 20ms), and it interprets the pulse width as a throttle command:
  - 1100 µs → stopped / neutral
  - 1940 µs → full speed

This is the standard RC servo/ESC protocol (typically 1000–2000 µs range). No relation to voltage.
*/
// Safety limits (absolute min/max to prevent ESC damage)
#define ESC_ABS_MIN_US      1100
#define ESC_ABS_MAX_US      1940

// Operational drive range (inset from abs limits for smoother start/stop)
#define ESC_DRIVE_MIN_US    1100
#define ESC_DRIVE_MAX_US    1940

// Low battery cutoff (disable if no battery sensor connected)
#define ENABLE_LOW_BATTERY_CUTOFF  false

// =============================================================================
// BATTERY SETTINGS (3S LiPo)
// =============================================================================

#define BATTERY_CELLS           3
#define BATTERY_MIN_VOLTAGE     10.0f   // Emergency stop (3.33V/cell)
#define BATTERY_LOW_VOLTAGE     10.8f   // Warning (3.6V/cell)
#define BATTERY_CRITICAL_VOLTAGE 10.2f  // Critical (3.4V/cell)
#define BATTERY_MAX_VOLTAGE     12.6f   // Fully charged (4.2V/cell)
#define BATTERY_ADC_RATIO       5.7f    // Voltage divider ratio

// =============================================================================
// Wi-Fi SETTINGS (Station / Client mode — joins an existing network)
// =============================================================================

#define WIFI_SSID                SECRET_WIFI_SSID      // From secrets.h
#define WIFI_PASSWORD            SECRET_WIFI_PASSWORD   // From secrets.h
#define WIFI_CONNECT_TIMEOUT_MS  15000                  // 15 seconds to connect

// =============================================================================
// WEB SERVER SETTINGS
// =============================================================================

#define WEB_SERVER_PORT             80
#define WEB_SERVER_MAX_SSE_CLIENTS  4       // only allow 4 clients to connect to live dashboard, otherwise Pico will cry (pls don't change)
#define WEB_SERVER_SSE_INTERVAL_MS  200     // interval for sending status updates to dashboard
#define WEB_SERVER_SSE_STALL_TIMEOUT_MS 3000 // drop SSE clients that stay backpressured for this long

// =============================================================================
// FOXGLOVE WEBSOCKET SETTINGS
// =============================================================================

#define FOXGLOVE_WS_PORT            8765    // the port Foxglove connects to (ws://<bot-ip>:8765)
#define FOXGLOVE_WS_MAX_CLIENTS     2       // each client costs an RX buffer, so keep this small
#define FOXGLOVE_WS_INTERVAL_MS     200     // telemetry broadcast interval, same rate as the SSE stream
#define FOXGLOVE_WS_STALL_TIMEOUT_MS 3000   // drop clients that stay backpressured for this long

// =============================================================================
// CONTROLLER MAPPING
// =============================================================================

// Xbox controller stick ranges
#define STICK_MIN    -512
#define STICK_MAX     511
#define TRIGGER_MAX   1023

// Invert controls if needed (-1 to invert, 1 for normal)
#define THROTTLE_INVERT  -1  // Push forward = forward (Y axis inverted)
#define TURN_INVERT       1  // Push right = turn right

// Expo curve for forward/back input (0.0 = linear, 1.0 = full cubic)
#define DRIVE_EXPO  0.6f

// Expo curve for turn input — higher = more dead zone around center
#define TURN_EXPO   0.8f

// Scale factor for turn input (0.0 = no turn, 1.0 = full authority)
#define TURN_SCALE  0.3f

// =============================================================================
// DEBUG SETTINGS
// =============================================================================

#define SERIAL_LOGGING      false

#endif // CONFIG_H
