// main/main.c
// FoloToy AI Passport — offline-first English study application.
//
// The demo menu and demo pages are intentionally not linked into this
// application. All input and state handling lives in langstudy_app.c and the
// notebook-style UI lives in langstudy_ui.c.
#include "langstudy_app.h"

#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = ls_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "application failed to start: %s", esp_err_to_name(err));
    }
}
