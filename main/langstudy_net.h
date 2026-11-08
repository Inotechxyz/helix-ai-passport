// main/langstudy_net.h
// Wi-Fi lifecycle for the application layer: STA connection, temporary SoftAP
// for phone setup (APSTA), and SNTP. The stack is fully torn down by
// ls_net_shutdown() so RAM can be reused between online sessions.
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LS_NET_IDLE = 0,
    LS_NET_AP,
    LS_NET_STA_CONNECTING,
    LS_NET_STA_CONNECTED,
    LS_NET_FAILED,
} ls_net_state_t;

// Initialize NVS/netif/event loop. Idempotent.
esp_err_t ls_net_prepare(void);

// Start APSTA mode with the given SoftAP credentials. Call before
// ls_net_sta_connect() when provisioning. Password length must be 0 (open) or
// 8..63; anything else is rejected.
esp_err_t ls_net_ap_start(const char *ssid, const char *password);

// Bring up (if needed) and connect the station. Blocks until GOT_IP or
// timeout. On failure the AP, if any, stays available for another attempt.
esp_err_t ls_net_sta_connect(const char *ssid, const char *password,
                             uint32_t timeout_ms);

bool ls_net_sta_connected(void);

// Last Wi-Fi disconnect reason code (wifi_err_reason_t) captured by the event
// handler, or 0 when the last connection succeeded or no attempt has run.
// Lets the setup page explain *why* a test failed instead of logging a timeout.
uint8_t ls_net_last_disconnect_reason(void);

// Block until SNTP sets the system clock. Requires an active STA connection.
esp_err_t ls_net_sync_time(uint32_t timeout_ms);

// Stop Wi-Fi and destroy the netifs. Safe to call repeatedly.
esp_err_t ls_net_shutdown(void);

ls_net_state_t ls_net_state(void);
