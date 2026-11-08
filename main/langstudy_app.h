// main/langstudy_app.h
// Boot sequence and the single input/tick task for the language-study app.
#pragma once

#include "esp_err.h"

// Initialize peripherals, load persisted state, build the UI, and start the
// input task. Returns ESP_OK once the application is running. The task keeps
// running after this function returns.
esp_err_t ls_app_start(void);
