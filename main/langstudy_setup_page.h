// main/langstudy_setup_page.h
// ASCII-only setup page served by the SoftAP provisioning server. Values are
// HTML-escaped; the stored Wi-Fi password and API key are never echoed back.
#pragma once

#include "langstudy_config.h"

#include <stddef.h>

#define LS_SETUP_PAGE_MAX 6144

// Returns the number of bytes written (excluding the NUL). Returns 0 if the
// output does not fit.
size_t ls_setup_page_render(char *out, size_t cap,
                            const ls_config_t *config,
                            const char *ap_ssid, const char *setup_url);
