// main/langstudy_online.c
#include "langstudy_online.h"

#include "langstudy_audio.h"
#include "langstudy_content.h"
#include "langstudy_deepseek.h"
#include "langstudy_gen.h"
#include "langstudy_net.h"
#include "langstudy_state.h"
#include "langstudy_store.h"
#include "langstudy_sync.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ONLINE_TASK_STACK 10240
#define ONLINE_TASK_PRIO 4
#define ONLINE_CONNECT_TIMEOUT_MS 30000
#define ONLINE_SNTP_TIMEOUT_MS 8000

typedef struct {
    ls_online_op_t op;
    ls_config_t config;
    uint32_t pack_crc;
    ls_progress_t *progress;  // owned by the worker for SYNC
    char avoid_words[LS_AVOID_WORDS_MAX];  // active pack + recent history
    uint32_t seed;            // fresh sample selector for this generation
    uint8_t level;            // 1..10 English level start + progress boost
} online_request_t;

static const char *TAG = "ls_online";

static QueueHandle_t s_requests;
static QueueHandle_t s_results;
static TaskHandle_t s_task;
static volatile bool s_busy;

static void post_message(ls_online_msg_kind_t kind, const char *message,
                         const char *detail, ls_gen_pack_t *pack,
                         ls_progress_t *progress, uint8_t link_flags)
{
    ls_online_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = kind;
    msg.pack = pack;
    msg.progress = progress;
    msg.link_flags = link_flags;
    snprintf(msg.message, sizeof(msg.message), "%s",
             message ? message : "");
    snprintf(msg.detail, sizeof(msg.detail), "%s", detail ? detail : "");
    if (!s_results || xQueueSend(s_results, &msg, 0) != pdTRUE) {
        free(pack);
        free(progress);
    }
    s_busy = false;
}

static bool word_has_progress(const ls_progress_t *progress, uint32_t word_id)
{
    if (!progress) return false;
    for (uint16_t i = 0;
         i < progress->record_count && i < LS_PROGRESS_RECORDS_MAX; i++) {
        if (progress->records[i].word_id == word_id) return true;
    }
    return false;
}

static void append_avoid_word(const char *word, char *out, size_t cap,
                              size_t *used)
{
    size_t len = strlen(word);
    if (len == 0) return;
    size_t need = len + (*used ? 1 : 0);
    if (*used + need + 1 > cap) return;  // stop on a word boundary
    if (*used) out[(*used)++] = ' ';
    memcpy(out + *used, word, len);
    *used += len;
    out[*used] = '\0';
}

// True when `word` appears in the space-separated avoid list. Comparison is
// case-insensitive because the model may change capitalization.
static bool word_in_avoid_list(const char *word, const char *list)
{
    if (!word || !list) return false;
    size_t word_len = strlen(word);
    if (word_len == 0) return false;

    const char *p = list;
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - start);
        if (len == word_len && strncasecmp(start, word, len) == 0) return true;
    }
    return false;
}

// Walk the rolling history newest-first (it is stored oldest-first) and append
// any word the list does not already hold. Newest-first matters: when the cap
// runs out, the most recent packs keep their exclusion.
static void append_history_words(const char *history, char *out, size_t cap,
                                 size_t *used)
{
    if (!history || !history[0]) return;
    size_t end = strlen(history);
    char word[LS_WORD_TEXT_MAX + 1];

    while (end > 0) {
        while (end > 0 && history[end - 1] == ' ') end--;
        if (end == 0) break;
        size_t start = end;
        while (start > 0 && history[start - 1] != ' ') start--;
        size_t len = end - start;
        if (len > 0 && len <= LS_WORD_TEXT_MAX) {
            memcpy(word, history + start, len);
            word[len] = '\0';
            if (!word_in_avoid_list(word, out)) {
                append_avoid_word(word, out, cap, used);
            }
        }
        end = start;
    }
}

// Snapshot the active pack's words plus the rolling generation history into one
// space-separated string. Words the learner has actually answered come first:
// if an unusually long list ever overflows, the refresh still excludes
// everything already studied. The history follows the active pack so words the
// learner never saw in the current pack keep being excluded as well. Built on
// the UI thread while the worker only reads the copy.
static void build_avoid_words(const ls_pack_t *pack,
                              const ls_progress_t *progress,
                              const char *history, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!pack) return;

    size_t used = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < pack->word_count && i < LS_WORDS_MAX; i++) {
            bool studied = word_has_progress(progress, pack->words[i].id);
            if ((pass == 0) != studied) continue;
            append_avoid_word(pack->words[i].text, out, cap, &used);
        }
    }
    append_history_words(history, out, cap, &used);
}

// Copy up to a handful of repeated words into `out` for the detail screen.
// Returns the total number of repeats (which may exceed what fits).
static uint16_t collect_repeated_words(const ls_word_t *words, uint16_t count,
                                       const char *avoid, char *out,
                                       size_t cap)
{
    uint16_t repeated = 0;
    size_t used = 0;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!words || !avoid || !avoid[0]) return 0;

    for (size_t i = 0; i < count; i++) {
        const char *word = words[i].text;
        if (!word_in_avoid_list(word, avoid)) continue;
        repeated++;
        size_t len = strlen(word);
        if (used + len + (used ? 1 : 0) + 1 > cap) continue;
        if (used) out[used++] = ' ';
        memcpy(out + used, word, len);
        used += len;
        out[used] = '\0';
    }
    return repeated;
}

static bool start_radio(const online_request_t *request, char *error,
                        size_t error_cap)
{
    esp_err_t err = ls_audio_release();
    if (err != ESP_OK) {
        snprintf(error, error_cap, "Could not release audio: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = ls_net_sta_connect(request->config.wifi_ssid,
                             request->config.wifi_pass,
                             ONLINE_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        snprintf(error, error_cap, "Wi-Fi failed: %s", esp_err_to_name(err));
        ls_net_shutdown();
        ls_audio_resume();
        return false;
    }

    // TLS certificate validation needs a plausible clock, so every HTTPS path
    // (refresh, the DeepSeek probe, and an https sync URL) syncs time first.
    bool need_time = (request->op == LS_ONLINE_REFRESH) ||
                     (request->op == LS_ONLINE_CHECK) ||
                     (strncmp(request->config.sync_url, "https://", 8) == 0);
    if (need_time) {
        err = ls_net_sync_time(ONLINE_SNTP_TIMEOUT_MS);
        if (err != ESP_OK) {
            snprintf(error, error_cap, "Clock sync failed: %s",
                     esp_err_to_name(err));
            ls_net_shutdown();
            ls_audio_resume();
            return false;
        }
    }
    return true;
}

static void stop_radio(void)
{
    ls_net_shutdown();
    ls_audio_resume();
}

static const char *deepseek_error_text(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NO_MEM: return "Out of memory";
    case ESP_ERR_INVALID_RESPONSE: return "DeepSeek reply incomplete, try again";
    case ESP_ERR_INVALID_SIZE: return "DeepSeek reply cut off, try again";
    case ESP_ERR_TIMEOUT: return "Network timeout, try again";
    case ESP_ERR_HTTP_CONNECT:
        return "Cannot reach DeepSeek (Wi-Fi/TLS)";
    default: return esp_err_to_name(err);
    }
}

// Append one line to a multi-line status message. Uses "\n" separators so the
// UI can keep one label for the whole connection report.
static void add_line(char *out, size_t cap, size_t *pos, const char *line)
{
    if (*pos >= cap - 1) return;
    int n = snprintf(out + *pos, cap - *pos, "%s%s", *pos ? "\n" : "", line);
    if (n < 0) return;
    size_t advance = (size_t)n;
    if (advance >= cap - *pos) {
        *pos = cap - 1;
        out[*pos] = '\0';
    } else {
        *pos += advance;
    }
}

static void do_refresh(online_request_t *request)
{
    char error[96];
    char detail[LS_ONLINE_DETAIL_MAX];
    detail[0] = '\0';
    if (request->config.api_key[0] == '\0') {
        post_message(LS_ONLINE_REFRESH_FAIL,
                     "Set the DeepSeek API key in setup",
                     "Phone setup -> DeepSeek API key", NULL, NULL, 0);
        return;
    }
    if (request->config.wifi_ssid[0] == '\0') {
        post_message(LS_ONLINE_REFRESH_FAIL, "Set up Wi-Fi first",
                     "Phone setup -> Wi-Fi network", NULL, NULL, 0);
        return;
    }
    if (!start_radio(request, error, sizeof(error))) {
        post_message(LS_ONLINE_REFRESH_FAIL, error, error, NULL, NULL, 0);
        return;
    }

    // The worker returns the response buffer itself, decoded in place into the
    // finalized pack text, so no extra content-sized buffer is live while TLS
    // is active.
    ls_deepseek_cfg_t cfg = {
        .api_key = request->config.api_key,
        .model = NULL,
        .timeout_ms = 0,
        .exam = request->config.exam,
        .avoid_words = request->avoid_words[0] ? request->avoid_words : NULL,
        .strict_avoid = false,
        .seed = request->seed,
        .level = request->level,
    };
    ESP_LOGI(TAG, "生成词包: seed=%08lX, 等级=%u/10, 排除词=%u 字节",
             (unsigned long)cfg.seed, (unsigned)cfg.level,
             (unsigned)strlen(cfg.avoid_words ? cfg.avoid_words : ""));

    // One automatic retry when the reply is malformed or truncated. Reused
    // vocabulary is accepted with a notice instead: a repeat is a quality
    // signal, not a reason to keep the learner waiting for another generation.
    ls_gen_pack_t *pack = NULL;
    esp_err_t err = ESP_OK;
    bool radio_up = true;
    for (int attempt = 0; attempt < 2; attempt++) {
        char *text = NULL;
        size_t len = 0;
        pack = NULL;
        detail[0] = '\0';
        cfg.strict_avoid = attempt > 0;
        // Retrying with the same seed invites the same sample again; mix the
        // attempt number into the seed so the retry explores a different slice
        // of the vocabulary.
        cfg.seed = request->seed ^ ((uint32_t)attempt * 0x9E3779B9u);

        err = ls_deepseek_generate(&cfg, &text, &len);

        // The HTTP client is already closed, but the Wi-Fi stack still holds
        // tens of kilobytes. Shut it down before allocating the parsed model:
        // on the SRAM-only ESP32-C3 keeping both live produced the "Out of
        // memory" failure reported after a 100-word refresh.
        stop_radio();
        radio_up = false;

        if (err == ESP_OK) {
            // Parse into the smaller staging model. The 100-word ls_pack_t is
            // 25 KiB and, with the 16 KiB reply still allocated, fragmentation
            // after a live TLS session made that allocation fail on this
            // SRAM-only board ("out of memory for parsed pack ... free=101504").
            // LS_GEN_WORDS_MAX (60) keeps the staging allocation near 15 KiB,
            // inside the size class the reply buffer itself already proved
            // available. The UI copies it into the active pack afterwards.
            pack = (ls_gen_pack_t *)malloc(sizeof(*pack));
            if (!pack) {
                // The radio is already down here, so a failure means the heap
                // is genuinely exhausted rather than fragmented by TLS.
                snprintf(detail, sizeof(detail),
                         "out of memory for parsed pack (%u bytes, free=%u)",
                         (unsigned)sizeof(*pack),
                         (unsigned)esp_get_free_heap_size());
                ESP_LOGE(TAG, "分配解析模型失败(%u 字节), 剩余堆=%u, "
                              "最大连续=%u 字节",
                         (unsigned)sizeof(*pack),
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_DEFAULT));
                err = ESP_ERR_NO_MEM;
            } else {
                ls_pack_view_t view = {
                    .lesson_cap = LS_LESSONS_MAX,
                    .word_cap = LS_GEN_WORDS_MAX,
                    .lessons = pack->lessons,
                    .words = pack->words,
                };
                ls_status_t parsed = ls_pack_parse_view(text, len, &view);
                pack->lesson_count = view.lesson_count;
                pack->word_count = view.word_count;
                if (parsed != LS_OK) {
                    snprintf(detail, sizeof(detail),
                             "pack parse failed (status %d, %u bytes): %.96s",
                             (int)parsed, (unsigned)len, text);
                    err = ESP_ERR_INVALID_RESPONSE;
                } else {
                    // Replace model-chosen ids with the stable FNV ids the
                    // progress records key on.
                    ls_pack_assign_ids(pack->words, pack->word_count);
                    if (!ls_pack_validate_view(&view)) {
                        snprintf(detail, sizeof(detail),
                                 "pack validation failed (%u words, %u lessons)",
                                 (unsigned)pack->word_count,
                                 (unsigned)pack->lesson_count);
                        err = ESP_ERR_INVALID_RESPONSE;
                    } else {
                        // Repeats are reported, not fatal. The pack is still an
                        // improvement over stalling at 100% completion, and the
                        // status screen names the words that came back.
                        char words[LS_ONLINE_DETAIL_MAX];
                        uint16_t repeated_count = collect_repeated_words(
                            pack->words, pack->word_count, request->avoid_words,
                            words, sizeof(words));
                        if (repeated_count > 0) {
                            // The list is already capped, but a precision keeps
                            // the prefix and list within the detail buffer.
                            snprintf(detail, sizeof(detail),
                                     "%u repeated word(s): %.160s",
                                     (unsigned)repeated_count, words);
                            ESP_LOGW(TAG, "刷新结果包含已学单词(%u), 仍接受",
                                     (unsigned)repeated_count);
                        }
                    }
                }
            }
        } else {
            snprintf(detail, sizeof(detail), "%s",
                     ls_deepseek_last_error());
        }
        free(text);

        if (err == ESP_OK) break;
        free(pack);
        pack = NULL;
        // A malformed or truncated reply is worth one more call because long
        // completions are sometimes cut short; connectivity and memory
        // failures are not retried.
        bool retry = err == ESP_ERR_INVALID_RESPONSE ||
                     err == ESP_ERR_INVALID_SIZE;
        if (!retry || attempt + 1 >= 2) break;
        ESP_LOGW(TAG, "刷新结果不可用(%s), 重试一次", esp_err_to_name(err));
        if (!start_radio(request, error, sizeof(error))) {
            post_message(LS_ONLINE_REFRESH_FAIL, error, error, NULL, NULL, 0);
            return;
        }
        radio_up = true;
    }

    if (radio_up) stop_radio();

    if (err == ESP_OK && pack) {
        // A truncated IELTS generation may be accepted with fewer than 60
        // words, so report the count actually stored. `detail` is empty unless
        // the reply reused vocabulary, in which case the UI shows that notice.
        char ready[32];
        snprintf(ready, sizeof(ready), "%u new words ready",
                 (unsigned)pack->word_count);
        post_message(LS_ONLINE_REFRESH_OK, ready, detail, pack, NULL,
                     LS_LINK_WIFI_KNOWN | LS_LINK_WIFI_OK |
                         LS_LINK_DEEPSEEK_KNOWN | LS_LINK_DEEPSEEK_OK);
        return;
    }

    free(pack);
    const char *message = deepseek_error_text(err);
    if (detail[0] == '\0') {
        snprintf(detail, sizeof(detail), "%s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "刷新单词失败: %s (%s) [%s]", message,
             esp_err_to_name(err), detail);
    post_message(LS_ONLINE_REFRESH_FAIL, message, detail, NULL, NULL,
                 LS_LINK_WIFI_KNOWN | LS_LINK_WIFI_OK |
                     LS_LINK_DEEPSEEK_KNOWN);
}

static void do_sync(online_request_t *request)
{
    char error[96];
    if (request->config.wifi_ssid[0] == '\0') {
        post_message(LS_ONLINE_SYNC_FAIL, "Set up Wi-Fi first",
                     "Phone setup -> Wi-Fi network", NULL, NULL, 0);
        return;
    }
    if (request->config.sync_url[0] == '\0') {
        post_message(LS_ONLINE_SYNC_FAIL, "Set a sync server URL in setup",
                     "Phone setup -> sync URL", NULL, NULL, 0);
        return;
    }
    if (!request->progress) {
        post_message(LS_ONLINE_SYNC_FAIL, "No progress to sync",
                     "Answer at least one card first", NULL, NULL, 0);
        return;
    }
    if (!start_radio(request, error, sizeof(error))) {
        post_message(LS_ONLINE_SYNC_FAIL, error, error, NULL, NULL, 0);
        return;
    }

    ls_progress_t *remote = (ls_progress_t *)malloc(sizeof(*remote));
    esp_err_t err;
    if (!remote) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = ls_sync_exchange(request->config.sync_url, request->pack_crc,
                               request->progress, remote, 0);
    }

    stop_radio();

    // Sync only proves that Wi-Fi and the configured server are reachable, so
    // only the Wi-Fi icon is updated; DeepSeek stays at its last known state.
    uint8_t wifi_flag = LS_LINK_WIFI_KNOWN | LS_LINK_WIFI_OK;
    if (err == ESP_OK) {
        post_message(LS_ONLINE_SYNC_OK, "Progress synced", NULL, NULL, remote,
                     wifi_flag);
    } else {
        free(remote);
        char detail[LS_ONLINE_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "%s: %s", request->config.sync_url,
                 esp_err_to_name(err));
        ESP_LOGW(TAG, "同步失败: %s", esp_err_to_name(err));
        post_message(LS_ONLINE_SYNC_FAIL, "Sync failed", detail, NULL, NULL,
                     wifi_flag);
    }
}

// Reports each hop separately so a failure points at Wi-Fi, the clock, or the
// API key instead of a single opaque "online failed" message.
static void do_check(online_request_t *request)
{
    char message[LS_ONLINE_MESSAGE_MAX];
    size_t pos = 0;
    message[0] = '\0';

    if (request->config.wifi_ssid[0] == '\0') {
        add_line(message, sizeof(message), &pos, "Wi-Fi: not configured");
        add_line(message, sizeof(message), &pos, "DeepSeek: not tested");
        post_message(LS_ONLINE_CHECK_FAIL, message, NULL, NULL, NULL, 0);
        return;
    }

    esp_err_t err = ls_audio_release();
    if (err != ESP_OK) {
        add_line(message, sizeof(message), &pos, "Audio release failed");
        post_message(LS_ONLINE_CHECK_FAIL, message, NULL, NULL, NULL, 0);
        return;
    }

    err = ls_net_sta_connect(request->config.wifi_ssid,
                             request->config.wifi_pass,
                             ONLINE_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        char line[64];
        snprintf(line, sizeof(line), "Wi-Fi: FAIL (%s)",
                 esp_err_to_name(err));
        add_line(message, sizeof(message), &pos, line);
        add_line(message, sizeof(message), &pos, "DeepSeek: not tested");
        ls_net_shutdown();
        ls_audio_resume();
        post_message(LS_ONLINE_CHECK_FAIL, message, line, NULL, NULL,
                     LS_LINK_WIFI_KNOWN);
        return;
    }

    char line[96];
    snprintf(line, sizeof(line), "Wi-Fi: OK  %s", request->config.wifi_ssid);
    add_line(message, sizeof(message), &pos, line);

    bool time_ok = ls_net_sync_time(ONLINE_SNTP_TIMEOUT_MS) == ESP_OK;
    add_line(message, sizeof(message), &pos, time_ok ? "Clock: OK"
                                                     : "Clock: FAIL");

    uint8_t link = LS_LINK_WIFI_KNOWN | LS_LINK_WIFI_OK;
    bool all_ok = time_ok;
    if (request->config.api_key[0] == '\0') {
        add_line(message, sizeof(message), &pos, "DeepSeek: no API key");
        link |= LS_LINK_DEEPSEEK_KNOWN;  // definitely not usable
        all_ok = false;
    } else if (!time_ok) {
        // TLS verification would fail with an unset clock; skip the request
        // rather than reporting a misleading DeepSeek error.
        add_line(message, sizeof(message), &pos, "DeepSeek: not tested");
        all_ok = false;
    } else {
        ls_deepseek_cfg_t cfg = {
            .api_key = request->config.api_key,
            .model = NULL,
            .timeout_ms = 0,
        };
        int status = 0;
        err = ls_deepseek_probe(&cfg, 0, &status);
        link |= LS_LINK_DEEPSEEK_KNOWN;
        if (err == ESP_OK) {
            add_line(message, sizeof(message), &pos, "DeepSeek: OK");
            link |= LS_LINK_DEEPSEEK_OK;
        } else if (status > 0) {
            snprintf(line, sizeof(line), "DeepSeek: HTTP %d", status);
            add_line(message, sizeof(message), &pos, line);
            all_ok = false;
        } else {
            snprintf(line, sizeof(line), "DeepSeek: FAIL (%s)",
                     esp_err_to_name(err));
            add_line(message, sizeof(message), &pos, line);
            all_ok = false;
        }
    }

    stop_radio();
    post_message(all_ok ? LS_ONLINE_CHECK_OK : LS_ONLINE_CHECK_FAIL, message,
                 NULL, NULL, NULL, link);
}

static void online_task(void *arg)
{
    (void)arg;
    online_request_t request;
    for (;;) {
        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (request.op) {
        case LS_ONLINE_REFRESH: do_refresh(&request); break;
        case LS_ONLINE_CHECK: do_check(&request); break;
        default: do_sync(&request); break;
        }
        free(request.progress);
        request.progress = NULL;
        memset(&request, 0, sizeof(request));
    }
}

esp_err_t ls_online_init(void)
{
    if (s_requests) return ESP_OK;

    s_requests = xQueueCreate(1, sizeof(online_request_t));
    s_results = xQueueCreate(2, sizeof(ls_online_msg_t));
    if (!s_requests || !s_results) {
        if (s_requests) vQueueDelete(s_requests);
        if (s_results) vQueueDelete(s_results);
        s_requests = NULL;
        s_results = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(online_task, "ls_online", ONLINE_TASK_STACK, NULL,
                    ONLINE_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_requests);
        vQueueDelete(s_results);
        s_requests = NULL;
        s_results = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_busy = false;
    return ESP_OK;
}

void ls_online_shutdown(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_requests) {
        online_request_t request;
        while (xQueueReceive(s_requests, &request, 0) == pdTRUE) {
            free(request.progress);
        }
        vQueueDelete(s_requests);
        s_requests = NULL;
    }
    if (s_results) {
        ls_online_msg_t msg;
        while (xQueueReceive(s_results, &msg, 0) == pdTRUE) {
            free(msg.pack);
            free(msg.progress);
        }
        vQueueDelete(s_results);
        s_results = NULL;
    }
    // The worker may have been deleted mid-request; make sure the radio is off
    // before the caller powers down I2C/display for deep sleep.
    ls_net_shutdown();
    s_busy = false;
}

bool ls_online_start(ls_online_op_t op)
{
    if (!s_requests || s_busy) return false;

    online_request_t request;
    memset(&request, 0, sizeof(request));
    request.op = op;
    request.config = ls_state()->config;
    request.pack_crc = ls_state()->pack_crc;

    if (op == LS_ONLINE_REFRESH) {
        // Snapshot the current pack's words and the recent-generation history
        // while the UI thread owns them. A fresh random seed keeps repeated
        // refreshes from converging on the same sample; the English level
        // starts at the SETTINGS value and rises with accumulated sessions and
        // answers.
        build_avoid_words(&ls_state()->pack, &ls_state()->progress,
                          ls_state()->avoid_history, request.avoid_words,
                          sizeof(request.avoid_words));
        request.seed = esp_random();
        request.level = ls_gen_level_for_progress(
            ls_state()->config.level, ls_state()->progress.answered_total,
            ls_state()->progress.session_count);
    }

    if (op == LS_ONLINE_SYNC) {
        request.progress = (ls_progress_t *)malloc(sizeof(*request.progress));
        if (!request.progress) return false;
        *request.progress = ls_state()->progress;
    }

    s_busy = true;
    if (xQueueSend(s_requests, &request, 0) != pdTRUE) {
        free(request.progress);
        s_busy = false;
        return false;
    }
    return true;
}

bool ls_online_poll(ls_online_msg_t *msg)
{
    if (!s_results || !msg) return false;
    return xQueueReceive(s_results, msg, 0) == pdTRUE;
}

bool ls_online_busy(void)
{
    return s_busy;
}
