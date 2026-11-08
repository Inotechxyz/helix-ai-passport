// main/langstudy_config.h
// Non-secret-looking settings live in NVS under the shared "langstudy"
// namespace. The API key is stored as a plain NVS string: it is never logged,
// never shown back in full on screen, and users can clear it from the setup
// page. Devices that need at-rest encryption should add NVS encryption; that is
// deliberately out of scope for this reference application.
#pragma once

#include "langstudy_model.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_SSID_MAX 32
#define LS_WIFI_PASS_MAX 63
#define LS_API_KEY_MAX 200
#define LS_SYNC_URL_MAX 160

// User-facing preferences. Brightness and volume move in LS_LEVEL_STEP steps.
#define LS_BRIGHTNESS_DEFAULT 80
#define LS_BRIGHTNESS_MIN 10
#define LS_VOLUME_DEFAULT 80
#define LS_LEVEL_STEP 10

// Auto-play holds each word for this many seconds; adjusted in 1 s steps.
#define LS_WORD_DELAY_DEFAULT_S 5
#define LS_WORD_DELAY_MIN_S 2
#define LS_WORD_DELAY_MAX_S 15

// English level: 1 = starter, 10 = professional. It is the starting point for
// word retrieval; progress raises the effective level from there.
#define LS_ENGLISH_LEVEL_DEFAULT 4
#define LS_ENGLISH_LEVEL_MIN 1
#define LS_ENGLISH_LEVEL_MAX 10

typedef struct {
    char wifi_ssid[LS_SSID_MAX + 1];
    char wifi_pass[LS_WIFI_PASS_MAX + 1];
    char api_key[LS_API_KEY_MAX + 1];
    char sync_url[LS_SYNC_URL_MAX + 1];

    ls_exam_t exam;        // vocabulary tier used by the next refresh
    uint8_t level;         // English level, LS_ENGLISH_LEVEL_MIN..MAX
    uint8_t brightness;    // backlight percent, LS_BRIGHTNESS_MIN..100
    uint8_t volume;        // speaker percent, 0..100
    uint8_t word_delay_s;  // auto-play dwell per word, seconds
} ls_config_t;

// NVS must already be initialized (bsp_storage_init). Missing keys leave the
// corresponding field empty. Returns ESP_OK even when nothing was stored yet.
esp_err_t ls_config_load(ls_config_t *out);

// Persist individual settings. Empty strings erase the key.
esp_err_t ls_config_save_network(const char *ssid, const char *password);
esp_err_t ls_config_save_api_key(const char *api_key);
esp_err_t ls_config_save_sync_url(const char *sync_url);

// Persist the vocabulary tier selected in REFRESH WORDS.
esp_err_t ls_config_save_exam(ls_exam_t exam);
// Persist English level, display, audio and auto-play preferences in one
// commit.
esp_err_t ls_config_save_preferences(uint8_t level, uint8_t brightness,
                                     uint8_t volume, uint8_t word_delay_s);

// Persist every field in one commit.
esp_err_t ls_config_save_all(const ls_config_t *config);

esp_err_t ls_config_clear_network(void);
esp_err_t ls_config_clear_all(void);

static inline bool ls_config_has_wifi(const ls_config_t *config)
{
    return config && config->wifi_ssid[0] != '\0';
}

static inline bool ls_config_has_api_key(const ls_config_t *config)
{
    return config && config->api_key[0] != '\0';
}

static inline bool ls_config_has_sync_url(const ls_config_t *config)
{
    return config && config->sync_url[0] != '\0';
}
