/**
 * @file foxglove_ws.h
 * @brief Foxglove WebSocket server that runs on the bot.
 *
 * The bot is the server. Open Foxglove, choose "Open connection", and enter
 * `ws://<bot-ip>:8765`. No bridge program and no host tooling are needed.
 *
 * The server speaks the `foxglove.sdk.v1` subprotocol. It advertises five
 * channels, all with JSON messages and JSON Schema definitions:
 *
 *   - `/tf`       `foxglove.FrameTransform`  drives the 3D panel
 *   - `/motors`   `battlebot.Motors`
 *   - `/imu`      `battlebot.Imu`
 *   - `/thermal`  `battlebot.Thermal`
 *   - `/state`    `battlebot.State`
 *
 * The server also accepts client publishes on `/cmd/estop`, `/cmd/test`, and
 * `/cmd/test_stop`. These are for bench testing only. Do not use them as a
 * real emergency stop.
 *
 * Wi-Fi must be connected and @ref web_server_init must have run before you
 * call @ref foxglove_ws_init, because this module reads telemetry through
 * @ref web_server_get_snapshot.
 */
#ifndef FOXGLOVE_WS_H
#define FOXGLOVE_WS_H

#include <stdbool.h>
#include <stdint.h>

#include "motor_controller.h"

/** Bytes held per client while a message is still incomplete. */
#define FOXGLOVE_WS_RX_BUFFER_SIZE 1024

/** Bytes for the largest message the server builds, which is the channel list. */
#define FOXGLOVE_WS_TX_BUFFER_SIZE 4096

/** Bytes for one telemetry message, including its 13 byte binary header. */
#define FOXGLOVE_WS_MSG_BUFFER_SIZE 640

/** Command topics one client may advertise at the same time. */
#define FOXGLOVE_WS_MAX_CLIENT_CHANNELS 4

/**
 * @brief Start the Foxglove WebSocket server.
 * @param motors  Motor controller, used by the bench command topics.
 * @return true on success.
 */
bool foxglove_ws_init(motor_controller_t* motors);

/** @brief Stop the server and close every client connection. */
void foxglove_ws_stop(void);

/** @brief Return true if the server is running. */
bool foxglove_ws_is_running(void);

/** @brief Return the number of connected Foxglove clients. */
int foxglove_ws_client_count(void);

#endif // FOXGLOVE_WS_H
