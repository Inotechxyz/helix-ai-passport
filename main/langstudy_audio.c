// main/langstudy_audio.c
#include "langstudy_audio.h"

#include "bsp_audio.h"
#include "langstudy_config.h"
#include "langstudy_model.h"
#include "langstudy_synth.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

#define AUDIO_TASK_STACK 4096
#define AUDIO_TASK_PRIO 4
#define AUDIO_CHUNK_SAMPLES 256
#define AUDIO_RELEASE_TIMEOUT_MS 2500

static const char *TAG = "ls_audio";

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_mutex;
static volatile bool s_suspended;
static volatile bool s_released;
static volatile bool s_cancel;
static volatile bool s_speaking;
static volatile bool s_stream;
static bool s_hw_ready;
static uint8_t s_volume = LS_VOLUME_DEFAULT;

// Opens the codec if needed and makes sure it runs at sample_rate_hz. Requesting
// the rate that is already active is free (bsp_audio_set_format() returns early),
// so the 24 kHz online stream and the 16 kHz offline voice can share the device.
static bool ensure_hardware(uint32_t sample_rate_hz)
{
    if (!s_hw_ready) {
        esp_err_t err = bsp_audio_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "音频初始化失败: %s", esp_err_to_name(err));
            return false;
        }
        s_hw_ready = true;
    }
    esp_err_t err = bsp_audio_set_format(sample_rate_hz, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频格式设置失败 (%u Hz): %s", (unsigned)sample_rate_hz,
                 esp_err_to_name(err));
        (void)bsp_audio_deinit();
        s_hw_ready = false;
        return false;
    }
    bsp_audio_set_volume(s_volume);
    return true;
}

static void play_pronunciation(const char *pron)
{
    ls_tts_t tts;
    uint32_t seed = (uint32_t)esp_timer_get_time();
    if (!ls_tts_start(&tts, pron, seed)) return;

    int16_t pcm[AUDIO_CHUNK_SAMPLES];
    for (;;) {
        if (s_released || s_cancel) break;
        size_t samples = ls_tts_render(&tts, pcm, AUDIO_CHUNK_SAMPLES);
        if (samples > 0) {
            esp_err_t err = bsp_audio_write(pcm, samples * sizeof(int16_t));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "I2S 写入失败: %s", esp_err_to_name(err));
                break;
            }
        }
        if (samples < AUDIO_CHUNK_SAMPLES) break;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    char pron[LS_PRON_MAX + 1];
    for (;;) {
        if (xQueueReceive(s_queue, pron, portMAX_DELAY) != pdTRUE) continue;
        if (s_suspended) continue;

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) continue;
        // An online stream owns the hardware; drop the queued offline word.
        if (!s_suspended && !s_stream && ensure_hardware(LS_TTS_SAMPLE_RATE)) {
            s_speaking = true;
            play_pronunciation(pron);
            s_speaking = false;
        }
        // Do not clear a cancellation that belongs to an active online stream.
        if (!s_stream) s_cancel = false;
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t ls_audio_init(void)
{
    if (s_queue) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(1, LS_PRON_MAX + 1);
    if (!s_mutex || !s_queue) {
        if (s_mutex) vSemaphoreDelete(s_mutex);
        if (s_queue) vQueueDelete(s_queue);
        s_mutex = NULL;
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(audio_task, "ls_audio", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_mutex);
        s_queue = NULL;
        s_mutex = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_suspended = false;
    s_released = false;
    s_cancel = false;
    s_speaking = false;
    s_stream = false;
    return ESP_OK;
}

void ls_audio_shutdown(void)
{
    if (!s_queue) return;

    s_suspended = true;
    s_released = true;
    s_cancel = true;
    xQueueReset(s_queue);

    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_hw_ready) {
        (void)bsp_audio_deinit();
        s_hw_ready = false;
    }
    vQueueDelete(s_queue);
    vSemaphoreDelete(s_mutex);
    s_queue = NULL;
    s_mutex = NULL;
    s_speaking = false;
    s_stream = false;
}

bool ls_audio_speak(const char *pron)
{
    if (!s_queue || !pron || pron[0] == '\0' || s_suspended) return false;
    size_t len = strlen(pron);
    if (len > LS_PRON_MAX) return false;

    char item[LS_PRON_MAX + 1];
    memcpy(item, pron, len + 1);
    return xQueueSend(s_queue, item, 0) == pdTRUE;
}

void ls_audio_stop(void)
{
    if (!s_queue) return;
    s_cancel = true;
    xQueueReset(s_queue);
}

esp_err_t ls_audio_release(void)
{
    if (!s_queue) return ESP_OK;

    s_suspended = true;
    s_released = true;
    xQueueReset(s_queue);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(AUDIO_RELEASE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "等待播放停止超时,保留已打开的资源");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    s_stream = false;
    s_speaking = false;
    if (s_hw_ready) {
        err = bsp_audio_deinit();
        s_hw_ready = false;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "释放音频资源失败: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

void ls_audio_resume(void)
{
    if (!s_queue) return;
    s_cancel = true;
    s_released = false;
    s_suspended = false;
}

bool ls_audio_is_speaking(void)
{
    return s_speaking;
}

esp_err_t ls_audio_stream_begin(void)
{
    return ls_audio_stream_begin_rate(LS_TTS_SAMPLE_RATE);
}

esp_err_t ls_audio_stream_begin_rate(uint32_t sample_rate_hz)
{
    if (!s_queue) return ESP_FAIL;
    if (sample_rate_hz == 0) return ESP_ERR_INVALID_ARG;
    if (s_suspended || s_released) return ESP_ERR_INVALID_STATE;

    // A queued offline word must not start under the online stream.
    s_cancel = true;
    xQueueReset(s_queue);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (s_suspended || s_released) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!ensure_hardware(sample_rate_hz)) {
        err = ESP_FAIL;
    } else {
        s_cancel = false;
        s_stream = true;
        s_speaking = true;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t ls_audio_stream_write(const int16_t *pcm, size_t samples)
{
    if (!s_queue || !pcm || samples == 0) return ESP_ERR_INVALID_ARG;
    if (s_suspended || s_released || s_cancel) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_stream && !s_released && !s_suspended && !s_cancel) {
        err = bsp_audio_write(pcm, samples * sizeof(int16_t));
    }
    xSemaphoreGive(s_mutex);
    return err;
}

void ls_audio_stream_end(void)
{
    if (!s_queue) return;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    s_stream = false;
    s_speaking = false;
    xSemaphoreGive(s_mutex);
}

void ls_audio_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent;
    // Applying it while suspended would touch a closed codec; ensure_hardware()
    // reads s_volume when playback next opens the hardware.
    if (s_hw_ready && !s_suspended) bsp_audio_set_volume(s_volume);
}

uint8_t ls_audio_get_volume(void)
{
    return s_volume;
}
