// main/langstudy_ui.h
// The "PASSPORT ENGLISH" application UI.
//
// Three buttons, one 240x320 screen, single task:
//   UP   short  hear the word again
//   OK   short  I knew it  (counts as correct, advances)
//   DOWN short  show it again (counts as wrong, advances)
//   OK   long   open the menu
//
// All public entry points take the LVGL lock internally; call them from the
// application input task only. Online/refresh results are pumped from
// ls_ui_tick(), and the tick also drives idle dimming and deep-sleep timing.
#pragma once

#include "bsp_button.h"

#include "esp_err.h"

#include <stdbool.h>

// Build the notebook-style screens and show the first card. The application
// must have loaded state (pack/progress/config) before calling this.
esp_err_t ls_ui_init(void);

// Dispatch one button event. PRESS/RELEASE events only refresh the idle timer.
void ls_ui_on_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// Called periodically (about every 200 ms) from the application task. Pumps
// online results, refreshes the battery percentage, and applies idle dimming.
void ls_ui_tick(void);

// True once the UI has been idle long enough that the application should enter
// deep sleep. Cleared by any button event.
bool ls_ui_idle_deep_sleep_due(void);

void ls_ui_shutdown(void);
