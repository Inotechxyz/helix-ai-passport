// main/langstudy_provision.h
// Phone setup over a temporary SoftAP. The phone joins the AP and opens the
// setup page to set Wi-Fi, the DeepSeek API key, and the optional sync URL.
// Wi-Fi credentials are tested with a real STA connection before they are
// persisted.
#pragma once

#include "langstudy_config.h"

#include "esp_err.h"

#include <stdbool.h>

typedef struct {
    char ap_ssid[33];
    char ap_password[17];
    char url[24];
} ls_provision_info_t;

// Starts APSTA + HTTP server. Updates *config in place after a successful
// save. Call ls_provision_stop() before starting an online session.
esp_err_t ls_provision_start(ls_config_t *config, ls_provision_info_t *info);

void ls_provision_stop(void);

bool ls_provision_active(void);
