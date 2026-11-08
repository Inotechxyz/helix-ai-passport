// main/langstudy_config.c
#include "langstudy_config.h"

#include "bsp_storage.h"

#include "esp_log.h"

#include <string.h>

#define LS_NVS_NAMESPACE "langstudy"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_API_KEY "api_key"
#define KEY_SYNC_URL "sync_url"
#define KEY_EXAM "exam"
#define KEY_ENGLISH_LEVEL "level"
#define KEY_BRIGHTNESS "bright"
#define KEY_VOLUME "volume"
#define KEY_WORD_DELAY "word_delay"
#define KEY_CONFIG_VERSION "cfg_ver"

// Bump when a stored preference must be re-seeded on existing devices. Version
// 2 re-applies the 80% speaker default because earlier firmware persisted 0%.
#define LS_CONFIG_VERSION 2

static const char *TAG = "ls_config";

static void load_string(nvs_handle_t handle, const char *key,
                        char *dst, size_t cap)
{
    if (cap == 0) return;
    dst[0] = '\0';
    size_t len = cap;
    esp_err_t err = bsp_storage_get_str(handle, key, dst, &len);
    if (err != ESP_OK || len == 0) {
        dst[0] = '\0';
        return;
    }
    // nvs_get_str already NUL-terminates when it fits; guard against a stale
    // terminator if the stored value filled the buffer exactly.
    dst[cap - 1] = '\0';
}

static esp_err_t save_string(nvs_handle_t handle, const char *key,
                             const char *value)
{
    if (!value || value[0] == '\0') return bsp_storage_erase(handle, key);
    return bsp_storage_set_str(handle, key, value);
}

static uint8_t clamp_level(uint8_t value, uint8_t minimum)
{
    if (value > 100) return 100;
    if (value < minimum) return minimum;
    return value;
}

static uint8_t clamp_word_delay(uint8_t seconds)
{
    if (seconds < LS_WORD_DELAY_MIN_S) return LS_WORD_DELAY_MIN_S;
    if (seconds > LS_WORD_DELAY_MAX_S) return LS_WORD_DELAY_MAX_S;
    return seconds;
}

static uint8_t clamp_english_level(uint8_t level)
{
    if (level < LS_ENGLISH_LEVEL_MIN) return LS_ENGLISH_LEVEL_MIN;
    if (level > LS_ENGLISH_LEVEL_MAX) return LS_ENGLISH_LEVEL_MAX;
    return level;
}

esp_err_t ls_config_load(ls_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->exam = LS_EXAM_CET4;
    out->level = LS_ENGLISH_LEVEL_DEFAULT;
    out->brightness = LS_BRIGHTNESS_DEFAULT;
    out->volume = LS_VOLUME_DEFAULT;
    out->word_delay_s = LS_WORD_DELAY_DEFAULT_S;

    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开配置命名空间失败: %s", esp_err_to_name(err));
        return err;
    }

    load_string(handle, KEY_WIFI_SSID, out->wifi_ssid, sizeof(out->wifi_ssid));
    load_string(handle, KEY_WIFI_PASS, out->wifi_pass, sizeof(out->wifi_pass));
    load_string(handle, KEY_API_KEY, out->api_key, sizeof(out->api_key));
    load_string(handle, KEY_SYNC_URL, out->sync_url, sizeof(out->sync_url));

    uint8_t value = 0;
    if (bsp_storage_get_u8(handle, KEY_EXAM, &value) == ESP_OK &&
        value < (uint8_t)LS_EXAM_COUNT) {
        out->exam = (ls_exam_t)value;
    }
    if (bsp_storage_get_u8(handle, KEY_BRIGHTNESS, &value) == ESP_OK) {
        out->brightness = clamp_level(value, LS_BRIGHTNESS_MIN);
    }
    if (bsp_storage_get_u8(handle, KEY_ENGLISH_LEVEL, &value) == ESP_OK) {
        out->level = clamp_english_level(value);
    }
    if (bsp_storage_get_u8(handle, KEY_VOLUME, &value) == ESP_OK) {
        out->volume = clamp_level(value, 0);
    }
    if (bsp_storage_get_u8(handle, KEY_WORD_DELAY, &value) == ESP_OK) {
        out->word_delay_s = clamp_word_delay(value);
    }

    // One-time re-seed for devices that stored the old 0% default. The version
    // key is written with the new value, so a user who later chooses 0% keeps
    // it. handle is opened NVS_READWRITE, so this commit is allowed here.
    uint8_t version = 0;
    if (bsp_storage_get_u8(handle, KEY_CONFIG_VERSION, &version) != ESP_OK ||
        version < LS_CONFIG_VERSION) {
        out->volume = LS_VOLUME_DEFAULT;
        (void)bsp_storage_set_u8(handle, KEY_VOLUME,
                                 clamp_level(out->volume, 0));
        (void)bsp_storage_set_u8(handle, KEY_CONFIG_VERSION, LS_CONFIG_VERSION);
        (void)bsp_storage_commit(handle);
        ESP_LOGI(TAG, "配置升级到第 %d 版, 音量恢复默认 %u%%",
                 LS_CONFIG_VERSION, (unsigned)out->volume);
    }

    bsp_storage_close(handle);
    return ESP_OK;
}

esp_err_t ls_config_save_network(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;

    err = save_string(handle, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        // An open network legitimately has an empty password. Keep the key
        // present (empty string) whenever an SSID is stored so the setup page
        // can tell "configured open network" from "no network".
        if (ssid && ssid[0] != '\0') {
            err = bsp_storage_set_str(handle, KEY_WIFI_PASS,
                                      password ? password : "");
        } else {
            err = bsp_storage_erase(handle, KEY_WIFI_PASS);
        }
    }
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

esp_err_t ls_config_save_api_key(const char *api_key)
{
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = save_string(handle, KEY_API_KEY, api_key);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

esp_err_t ls_config_save_sync_url(const char *sync_url)
{
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = save_string(handle, KEY_SYNC_URL, sync_url);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

esp_err_t ls_config_save_exam(ls_exam_t exam)
{
    if ((uint8_t)exam >= (uint8_t)LS_EXAM_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = bsp_storage_set_u8(handle, KEY_EXAM, (uint8_t)exam);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

esp_err_t ls_config_save_preferences(uint8_t level, uint8_t brightness,
                                     uint8_t volume, uint8_t word_delay_s)
{
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = bsp_storage_set_u8(handle, KEY_ENGLISH_LEVEL,
                             clamp_english_level(level));
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_BRIGHTNESS,
                                 clamp_level(brightness, LS_BRIGHTNESS_MIN));
    }
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_WORD_DELAY,
                                 clamp_word_delay(word_delay_s));
    }
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_VOLUME, clamp_level(volume, 0));
    }
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_CONFIG_VERSION, LS_CONFIG_VERSION);
    }
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

esp_err_t ls_config_save_all(const ls_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;

    err = save_string(handle, KEY_WIFI_SSID, config->wifi_ssid);
    if (err == ESP_OK) {
        err = (config->wifi_ssid[0] != '\0')
                  ? bsp_storage_set_str(handle, KEY_WIFI_PASS, config->wifi_pass)
                  : bsp_storage_erase(handle, KEY_WIFI_PASS);
    }
    if (err == ESP_OK) err = save_string(handle, KEY_API_KEY, config->api_key);
    if (err == ESP_OK) err = save_string(handle, KEY_SYNC_URL, config->sync_url);
    if (err == ESP_OK && (uint8_t)config->exam < (uint8_t)LS_EXAM_COUNT) {
        err = bsp_storage_set_u8(handle, KEY_EXAM, (uint8_t)config->exam);
    }
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_ENGLISH_LEVEL,
                                 clamp_english_level(config->level));
    }
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_BRIGHTNESS,
                                 clamp_level(config->brightness, LS_BRIGHTNESS_MIN));
    }
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_VOLUME,
                                 clamp_level(config->volume, 0));
    }
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_WORD_DELAY,
                                 clamp_word_delay(config->word_delay_s));
    }
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_CONFIG_VERSION, LS_CONFIG_VERSION);
    }
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

esp_err_t ls_config_clear_network(void)
{
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = bsp_storage_erase(handle, KEY_WIFI_SSID);
    if (err == ESP_OK) err = bsp_storage_erase(handle, KEY_WIFI_PASS);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

esp_err_t ls_config_clear_all(void)
{
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = bsp_storage_erase_all(handle);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}
