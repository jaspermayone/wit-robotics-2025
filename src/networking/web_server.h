/**
 * @file web_server.h
 * @brief Simple HTTP status-dashboard server.
 *
 * Wi-Fi must be connected before calling @ref web_server_init.
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "motor_controller.h"

#define WEB_SERVER_PAGE_BUFFER_SIZE 512
#define WEB_SERVER_EVENT_BUFFER_SIZE 160
#define WEB_SERVER_REQUEST_BUFFER_SIZE 256
#define WEB_SERVER_SSE_DELAY_LOG_THRESHOLD_MS 500
#define WEB_SERVER_SSE_BACKPRESSURE_LOG_INTERVAL_MS 1000

/** @brief One reading of every value the bot publishes. */
typedef struct {
    int left;                    /**< Left drive speed, -100..100. */
    int right;                   /**< Right drive speed, -100..100. */
    int weapon;                  /**< Weapon speed, 0..100. */
    p_state state;               /**< Controller state. */
    bool failsafe;               /**< True if the failsafe is engaged. */
    uint32_t age_ms;             /**< Time since the last controller command. */
    float roll;                  /**< IMU roll, degrees. */
    float pitch;                 /**< IMU pitch, degrees. */
    float yaw;                   /**< IMU yaw, degrees. */
    float accel_x;               /**< IMU acceleration on X, g. */
    float accel_y;               /**< IMU acceleration on Y, g. */
    float accel_z;               /**< IMU acceleration on Z, g. */
    float gyro_x;                /**< IMU angular rate on X, degrees per second. */
    float gyro_y;                /**< IMU angular rate on Y, degrees per second. */
    float gyro_z;                /**< IMU angular rate on Z, degrees per second. */
    bool test_active;            /**< True while a bench motor test runs. */
    const char* test_motor;      /**< Name of the motor under test. */
    int test_power;              /**< Test power, 0..100. */
    uint32_t test_remaining_ms;  /**< Time left in the test. */
    uint8_t temp_c;              /**< DHT11 temperature, degrees C. */
    uint8_t humidity;            /**< DHT11 relative humidity, percent. */
} web_server_snapshot_t;

/**
 * @brief Read every telemetry value at once.
 *
 * This also polls the DHT11 sensor, but not faster than every 3 seconds.
 * Both the SSE stream and the Foxglove WebSocket server use this function, so
 * the sensor is read in one place only.
 *
 * @param out  Receives the reading. Must not be NULL.
 */
void web_server_get_snapshot(web_server_snapshot_t* out);

/** @brief Return the text name of a controller state ("ACTIVE", "INIT", or "STOPPED"). */
const char* web_server_state_text(p_state state);

/**
 * @brief Initialize and start the web server.
 * @param motors  Motor controller used for the status display page.
 * @return true on success.
 */
bool web_server_init(motor_controller_t* motors);

/** @brief Poll for incoming HTTP requests (non-blocking; call each main-loop iteration). */
void web_server_poll(void);

/** @brief Stop the web server and release resources. */
void web_server_stop(void);

/** @brief Return true if the web server is currently running. */
bool web_server_is_running(void);

/** @brief Cancel any active web motor test and stop the motors. */
bool web_server_interrupt_test_run(void);

#endif // WEB_SERVER_H
