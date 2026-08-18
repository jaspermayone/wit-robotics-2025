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
