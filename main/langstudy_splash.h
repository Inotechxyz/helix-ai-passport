// main/langstudy_splash.h
// Five-second launch screen shown on power-on, reset and reboot (not on a
// deep-sleep key wake). It owns its own LVGL screen; the main UI is created
// behind it while the dedication and artwork are visible.
#pragma once

#include "esp_err.h"

#include <stdint.h>

// Creates and loads the splash screen. Returns ESP_ERR_TIMEOUT when the LVGL
// lock is unavailable; the caller then skips the timed wait.
esp_err_t ls_splash_show(void);

// Moves the loading bar. permille is clamped to 0..1000.
void ls_splash_set_progress(uint32_t permille);

// Replaces the "LOADING..." caption (plain ASCII; Montserrat 14). Used to
// report a fatal start-up failure while leaving the dedication visible.
void ls_splash_set_caption(const char *text);

// Deletes the splash screen. Call only after another screen is active.
void ls_splash_hide(void);
