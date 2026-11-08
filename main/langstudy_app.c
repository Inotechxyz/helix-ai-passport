// main/langstudy_app.c
// Power-on flow: peripherals -> persisted state -> UI -> button input/tick.
//
// The button callback only copies events into a queue. Everything else runs on
// the single application task, which also pumps online results and idle sleep.
#include "langstudy_app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "key_guard.h"
#include "langstudy_audio.h"
#include "langstudy_builtin.h"
#include "langstudy_config.h"
#include "langstudy_content.h"
#include "langstudy_deepseek.h"
#include "langstudy_gen.h"
#include "langstudy_net.h"
#include "langstudy_online.h"
#include "langstudy_voice.h"
#include "langstudy_progress.h"
#include "langstudy_provision.h"
#include "langstudy_state.h"
#include "langstudy_status_cache.h"
#include "langstudy_store.h"
#include "langstudy_ui.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <time.h>

#define LS_INPUT_QUEUE_DEPTH 8
#define LS_APP_TASK_STACK 6144
#define LS_APP_TASK_PRIO 5
#define LS_KEY_GUARD_MAX_US (10LL * 1000 * 1000)

static const char *TAG = "ls_app";

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_ready;
static key_guard_t s_key_guard;

// Runs on the shared esp_timer task. Enqueue and return.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;

    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "wake key released; buttons active");
        }
        return;
    }
    if (!s_ready || !s_input_queue) return;

    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void enter_deep_sleep(void)
{
    // The shared button pin is also the low-level wake source. Do not arm it
    // while a key is held: the condition would be true at the instant of sleep
    // and the device would wake immediately.
    int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        ESP_LOGW(TAG, "key held (%d mV); postponing deep sleep", mv);
        return;
    }

    ESP_LOGI(TAG, "idle timeout: entering deep sleep");
    ls_voice_shutdown();
    ls_audio_stop();
    ls_provision_stop();
    ls_online_shutdown();
    ls_ui_shutdown();

    int level = 0;
    esp_err_t err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button hand-off failed (%s); restarting",
                 esp_err_to_name(err));
        esp_restart();
    }
    if (level != 1) {
        ESP_LOGW(TAG, "key still down at sleep; wake may be immediate");
    }

    err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO,
                                            ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO wake arm failed: %s", esp_err_to_name(err));
    }

    // The wake is a reset, so the header can only show what was saved here.
    // Read the gauge while it is still awake, then let it sleep.
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        ls_status_cache_set_soc(soc);
    }

    // CW2017 and ES8311 share I2C: the fuel gauge must finish first.
    (void)bsp_battery_sleep();
    (void)bsp_audio_sleep();
    (void)bsp_audio_prepare_deep_sleep();
    (void)bsp_i2c_prepare_deep_sleep();

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "could not stop LVGL; restarting");
        esp_restart();
    }
    (void)bsp_display_prepare_deep_sleep();

    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start returned; restarting");
    esp_restart();
}

static void app_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(200)) == pdTRUE) {
            ls_ui_on_key(input.btn, input.event);
        }
        ls_ui_tick();
        if (ls_ui_idle_deep_sleep_due()) {
            enter_deep_sleep();
        }
    }
}

static bool load_state(void)
{
    ls_state_t *state = ls_state();

    state->storage_ready = (ls_store_init() == ESP_OK);
    if (state->storage_ready) {
        esp_err_t err = ls_config_load(&state->config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "config load failed: %s", esp_err_to_name(err));
        }
    }

    // Deep sleep reboots the chip, so the link markers would otherwise vanish
    // on every wake until the next online operation. Restore the last result
    // saved before the previous sleep; a cold boot has no cache and shows
    // nothing, which is the honest state there.
    ls_status_cache_t cached;
    if (ls_status_cache_get(&cached)) {
        state->wifi_ok = cached.wifi_ok;
        state->deepseek_ok = cached.deepseek_ok;
        ESP_LOGI(TAG, "header status restored (wifi=%d deepseek=%d soc=%d)",
                 (int)cached.wifi_ok, (int)cached.deepseek_ok, cached.soc);
    } else {
        ESP_LOGI(TAG, "header status cache empty");
    }

    if (!ls_store_load_pack(&state->pack, &state->store)) {
        ESP_LOGE(TAG, "no usable pack; trying the built-in body directly");
        if (!ls_builtin_load(&state->pack)) return false;
        state->store.slot = 0xFF;
        state->store.generation = 0;
        state->store.from_storage = false;
    }
    state->pack_crc = ls_pack_crc(&state->pack);

    if (state->storage_ready) {
        (void)ls_store_load_progress(&state->progress);
    } else {
        ls_progress_init(&state->progress);
    }

    // The profile id and buddy travel together: creating a profile also picks
    // a buddy. If the id survived but the buddy record did not (rare NVS
    // corruption), start a fresh buddy instead of leaving the menu unable to
    // reach the BUDDY screen.
    if (state->storage_ready) {
        (void)ls_store_load_profile_id(state->profile_id,
                                       sizeof(state->profile_id));
        state->buddy_valid = ls_store_load_buddy(&state->buddy);
        if (state->profile_id[0] != '\0' && !state->buddy_valid) {
            uint32_t hash = 2166136261u;
            for (const char *p = state->profile_id; *p; p++) {
                hash = (hash ^ (uint8_t)*p) * 16777619u;
            }
            ls_buddy_init(&state->buddy,
                          (uint8_t)(hash % LS_BUDDY_SPECIES_COUNT),
                          (uint8_t)((hash >> 8) % LS_BUDDY_NAME_COUNT));
            state->buddy_valid = true;
            (void)ls_store_save_buddy(&state->buddy);
            ESP_LOGW(TAG, "buddy record missing; started a fresh buddy");
        }
        ESP_LOGI(TAG, "profile: %s",
                 state->profile_id[0] ? state->profile_id : "(none)");
    }

    // The rolling avoid list survives pack replacement, so repeated refreshes
    // keep excluding vocabulary from earlier packs. On the first boot after
    // the list was introduced it is seeded with the active pack, which is what
    // older firmware implicitly excluded.
    if (state->storage_ready) {
        if (ls_store_load_avoid_history(state->avoid_history,
                                        sizeof(state->avoid_history))) {
            ESP_LOGI(TAG, "avoid history: %u bytes",
                     (unsigned)strlen(state->avoid_history));
        } else {
            ls_gen_append_history(state->avoid_history,
                                  sizeof(state->avoid_history), &state->pack);
            (void)ls_store_save_avoid_history(state->avoid_history);
        }
    }

    ESP_LOGI(TAG, "content: %u lessons, %u words, crc=%08lX, source=%s",
             (unsigned)state->pack.lesson_count,
             (unsigned)state->pack.word_count,
             (unsigned long)state->pack_crc,
             state->store.from_storage ? "storage" : "built-in");
    return state->pack.word_count > 0;
}

esp_err_t ls_app_start(void)
{
    ESP_LOGI(TAG, "PASSPORT ENGLISH booting");

    const uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), LS_KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "key wake: suppressing actions until the key is released");
    }

    (void)bsp_i2c_init();

    // The header gauge reads the CW2017 every 30 s, but bsp_battery_soc()
    // returns -1 for as long as bsp_battery_init() has never run — which is
    // exactly what an empty outline on every boot looked like. Best effort:
    // a board without the fuel gauge simply keeps the empty outline, and the
    // deep-sleep path is unaffected because bsp_battery_sleep() no-ops too.
    esp_err_t batt = bsp_battery_init();
    if (batt != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge init failed (%s); header gauge stays empty",
                 esp_err_to_name(batt));
    }

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return ESP_FAIL;
    }
    // Full brightness while the panel comes up, then switch to the saved
    // preference once NVS has been read.
    bsp_display_backlight(100);

    if (!load_state()) {
        ESP_LOGE(TAG, "no content available; cannot start the UI");
        return ESP_FAIL;
    }

    const ls_config_t *config = &ls_state()->config;
    uint8_t brightness = config->brightness;
    if (!ls_state()->storage_ready || brightness < LS_BRIGHTNESS_MIN) {
        brightness = LS_BRIGHTNESS_DEFAULT;
    }
    bsp_display_backlight(brightness);

    if (ls_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio worker unavailable; running silently");
    }
    if (ls_voice_init() != ESP_OK) {
        ESP_LOGW(TAG, "voice worker unavailable; offline only");
    }
    ls_audio_set_volume(ls_state()->storage_ready ? config->volume
                                                  : LS_VOLUME_DEFAULT);
    if (ls_online_init() != ESP_OK) {
        ESP_LOGW(TAG, "online worker unavailable; offline only");
    }

    if (ls_ui_init() != ESP_OK) {
        ESP_LOGE(TAG, "UI init failed");
        return ESP_FAIL;
    }

    s_input_queue = xQueueCreate(LS_INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(app_task, "ls_app", LS_APP_TASK_STACK, NULL,
                    LS_APP_TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;

    esp_err_t err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_key_guard.armed) {
        // If the wake key came up before the driver started observing it, no
        // release event will ever arrive; sample the real level and disarm.
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }

    ESP_LOGI(TAG, "ready (storage=%d, audio/online best effort)",
             (int)ls_state()->storage_ready);

    return ESP_OK;
}
