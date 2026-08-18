/**
 * @file wifi_sta.c
 * @brief CYW43 Wi-Fi station (client) implementation — joins an existing network via DHCP.
 */

#include "wifi_sta.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"

static bool g_connected = false;
static char g_ip_str[16] = "0.0.0.0";

bool wifi_sta_init(void) {
    printf("Connecting to Wi-Fi network...\n");
    printf("  SSID: %s\n", WIFI_SSID);

    // Enable station (client) mode
    cyw43_arch_enable_sta_mode();

    printf("  Connecting (timeout %d s)...\n", WIFI_CONNECT_TIMEOUT_MS / 1000);

    int err = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK,
        WIFI_CONNECT_TIMEOUT_MS
    );

    if (err != 0) {
        printf("  FAILED to connect (error %d)\n", err);
        g_connected = false;
        return false;
    }

    // Grab the DHCP-assigned IP from the netif
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
    const ip4_addr_t *addr = netif_ip4_addr(nif);
    snprintf(g_ip_str, sizeof(g_ip_str), "%s", ip4addr_ntoa(addr));

    g_connected = true;

    printf("Wi-Fi connected!\n");
    printf("  IP address: %s\n", g_ip_str);
    printf("  Netmask:    %s\n", ip4addr_ntoa(netif_ip4_netmask(nif)));
    printf("  Gateway:    %s\n", ip4addr_ntoa(netif_ip4_gw(nif)));
    printf("  Browse to:  http://%s/\n", g_ip_str);

    return true;
}

bool wifi_sta_is_connected(void) {
    return g_connected;
}

const char* wifi_sta_get_ip(void) {
    return g_ip_str;
}

void wifi_sta_disconnect(void) {
    if (g_connected) {
        cyw43_arch_disable_sta_mode();
        g_connected = false;
        snprintf(g_ip_str, sizeof(g_ip_str), "0.0.0.0");
        printf("Wi-Fi disconnected\n");
    }
}
