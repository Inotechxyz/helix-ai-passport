// main/langstudy_voice.c
#include "langstudy_voice.h"

#include "langstudy_audio.h"
#include "langstudy_model.h"
#include "langstudy_net.h"
#include "langstudy_online.h"
#include "langstudy_state.h"
#include "langstudy_tts_edge.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdint.h>
#include <string.h>

#define TAG "ls_voice"
// mbedTLS handshakes plus the request/SSML buffers need room here. The MP3
// decode scratch is heap-allocated by langstudy_tts_edge, so it does not count.
#define VOICE_TASK_STACK 12288
#define VOICE_TASK_PRIO 4

// Wi-Fi is torn down after every online operation to save power. The voice
// worker brings it back for pronunciation and keeps it up briefly so tapping a
// few words in a row does not pay the connect cost each time.
#define VOICE_CONNECT_TIMEOUT_MS 6000
#define VOICE_NET_KEEPALIVE_MS 60000
#define VOICE_IDLE_TICK_MS 1000

typedef struct {
    char text[LS_WORD_TEXT_MAX + 1];
    char pron[LS_PRON_MAX + 1];
    uint32_t seq;
} voice_job_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_cancel;
static volatile uint32_t s_request_seq;
static bool s_net_owned;
static TickType_t s_net_last_use;

// The word currently being fetched or spoken. The Edge round trip is slow
// enough (a couple of seconds before the first sound) that learners tap UP
// again while the first request is still in flight; cancelling it there is
// what made the first attempt sound cut off.
static volatile bool s_active;
static char s_active_text[LS_WORD_TEXT_MAX + 1];
// Set when a repeat tap is absorbed while the same word is active. If that
// active attempt then fails, the worker replays it from its own copy.
static volatile bool s_repeat_requested;

bool ls_voice_cancelled(void)
{
    return s_cancel;
}

// Called on the voice worker only. Returns true when a station link is up.
static bool voice_net_ready(void)
{
    // Never race the online worker for the radio. This must come before the
    // connected check: during a check or probe the station link is up for the
    // whole SNTP/DeepSeek phase, and taking it then would run Edge TTS
    // concurrently with the worker that owns it.
    if (ls_online_probe_active()) {
        // The silent boot/wake probe only holds the radio for a few seconds,
        // so wait for it instead of making the first word after boot fall back
        // to the offline voice. Every other operation keeps the fallback.
        for (int i = 0; i < 100 && ls_online_probe_active(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (ls_online_busy()) return false;

    if (ls_net_sta_connected()) {
        s_net_last_use = xTaskGetTickCount();
        return true;
    }

    const ls_config_t *config = &ls_state()->config;
    if (!ls_config_has_wifi(config)) return false;

    // No decode buffers are reserved here on purpose: the Edge voice takes
    // them after its TLS handshake, which needs the largest contiguous block
    // the heap can offer.
    if (ls_net_prepare() != ESP_OK) return false;
    if (ls_net_sta_connect(config->wifi_ssid, config->wifi_pass,
                           VOICE_CONNECT_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    s_net_owned = true;
    s_net_last_use = xTaskGetTickCount();
    return true;
}

// Called on the voice worker only.
static void voice_net_release_if_idle(void)
{
    if (ls_online_busy()) {
        // The online worker needs every byte for its response and parse
        // buffers. Nothing is held between words, so this is normally a no-op.
        ls_edge_tts_unreserve();
        return;
    }
    if (!s_net_owned) {
        if (!ls_net_sta_connected()) ls_edge_tts_unreserve();
        return;
    }
    if ((xTaskGetTickCount() - s_net_last_use) * portTICK_PERIOD_MS <
        VOICE_NET_KEEPALIVE_MS) {
        return;
    }
    ESP_LOGI(TAG, "在线语音空闲,关闭 Wi-Fi");
    ls_net_shutdown();
    s_net_owned = false;
    ls_edge_tts_unreserve();
}

static void voice_task(void *arg)
{
    (void)arg;
    voice_job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, pdMS_TO_TICKS(VOICE_IDLE_TICK_MS)) !=
            pdTRUE) {
            voice_net_release_if_idle();
            continue;
        }

        for (;;) {
            s_cancel = false;
            s_repeat_requested = false;
            // Publish the active word before doing anything observable, so a
            // repeat tap on the same word is absorbed instead of cancelling
            // this attempt midway.
            strncpy(s_active_text, job.text, LS_WORD_TEXT_MAX);
            s_active_text[LS_WORD_TEXT_MAX] = '\0';
            s_active = true;

            bool spoken = false;
            if (voice_net_ready()) {
                s_net_last_use = xTaskGetTickCount();
                // Connecting can take seconds; a newer tap may have replaced
                // this job while we waited. Do not speak the stale word on top
                // of it.
                if (job.seq == s_request_seq) {
                    spoken = ls_edge_tts_play(job.text, ls_voice_cancelled);
                } else {
                    s_cancel = true;
                }
                s_net_last_use = xTaskGetTickCount();
            }
            s_active = false;
            if (!spoken && !s_cancel && job.pron[0] != '\0') {
                // The offline path also covers a caller that got here with no
                // network at all. Words without a stored pronunciation have
                // nothing to synthesize offline; the online voice uses the
                // text.
                (void)ls_audio_speak(job.pron);
            }

            // A repeat tap was absorbed while this attempt ran. Replay only if
            // the attempt produced nothing: the learner tapped again because
            // they did not hear the word, not because they wanted it twice.
            if (!spoken && !s_cancel && s_repeat_requested) {
                s_repeat_requested = false;
                ESP_LOGI(TAG, "重试一次在线发音");
                continue;
            }
            s_repeat_requested = false;
            break;
        }
    }
}

esp_err_t ls_voice_init(void)
{
    if (s_queue) return ESP_OK;

    s_queue = xQueueCreate(1, sizeof(voice_job_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreate(voice_task, "ls_voice", VOICE_TASK_STACK, NULL,
                    VOICE_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_cancel = false;
    return ESP_OK;
}

void ls_voice_stop(void)
{
    // Invalidate the in-flight job: the worker compares job.seq before it
    // starts, and the cancel flag aborts a request that is already running.
    s_cancel = true;
    s_request_seq++;
    s_repeat_requested = false;
    if (s_queue) xQueueReset(s_queue);
    ls_audio_stop();
}

void ls_voice_shutdown(void)
{
    if (!s_queue) return;
    s_cancel = true;
    ls_audio_stop();
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    vQueueDelete(s_queue);
    s_queue = NULL;
    s_net_owned = false;
    ls_edge_tts_unreserve();
}

bool ls_voice_speak(const char *text, const char *pron)
{
    if (!text || text[0] == '\0') {
        return ls_audio_speak(pron);
    }
    // Queue the online request even when the radio is currently down: the
    // worker reconnects with the stored credentials. No Wi-Fi configured means
    // there is nothing to try, so go straight to the offline voice.
    if (!s_queue || !ls_config_has_wifi(&ls_state()->config)) {
        return ls_audio_speak(pron);
    }

    // A repeat tap on the word that is already being fetched or spoken must
    // not cut it off: the online round trip often takes longer than the learner
    // expects, and cancelling here produced a half-spoken first attempt. The
    // worker replays it only if the attempt ends up producing no sound.
    if (s_active && strcmp(text, s_active_text) == 0) {
        s_repeat_requested = true;
        return true;
    }

    voice_job_t job;
    memset(&job, 0, sizeof(job));
    strncpy(job.text, text, LS_WORD_TEXT_MAX);
    if (pron && pron[0] != '\0') strncpy(job.pron, pron, LS_PRON_MAX);
    job.seq = ++s_request_seq;

    // A newer word wins: cancel the current playback and replace any pending
    // request in the single-slot queue.
    s_cancel = true;
    ls_audio_stop();
    if (xQueueOverwrite(s_queue, &job) != pdTRUE) {
        return ls_audio_speak(pron);
    }
    return true;
}
