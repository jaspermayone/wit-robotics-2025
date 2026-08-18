/**
 * @file wifi_sta.h
 * @brief Wi-Fi station (client) driver — connects to an existing network.
 */
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"  // All settings centralized

/** @brief Connect to the configured Wi-Fi network. @return true on success. */
bool wifi_sta_init(void);

/** @brief Return true if connected to the network. */
bool wifi_sta_is_connected(void);

/** @brief Return the assigned IPv4 address as a dotted-decimal string (valid after connect). */
const char* wifi_sta_get_ip(void);

/** @brief Disconnect from the Wi-Fi network. */
void wifi_sta_disconnect(void);

#endif // WIFI_STA_H
