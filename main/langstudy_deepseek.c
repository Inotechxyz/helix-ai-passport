// main/langstudy_deepseek.c
#include "langstudy_deepseek.h"

#include "langstudy_config.h"
#include "langstudy_gen.h"
#include "langstudy_json.h"
#include "langstudy_model.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEEPSEEK_URL "https://api.deepseek.com/chat/completions"
#define DEEPSEEK_MODELS_URL "https://api.deepseek.com/models"
#define DEFAULT_MODEL "deepseek-chat"
// Base prompt plus the optional avoid-list. Prompt construction happens
// before TLS. The request is written to the socket in 256-byte chunks, so the
// prompt is the only request-side allocation and it is released as soon as the
// body has been sent.
#define PROMPT_CAP (LS_GEN_PROMPT_BASE_MAX + LS_AVOID_WORDS_MAX)
#define PROBE_TIMEOUT_MS 10000
#define PROBE_DRAIN_MAX 4096
// Room for a 60-word pack plus the {"pack":"..."} wrapper some responses use.
// The wrapper carries the pack with its newlines escaped, so leave a little
// more than the raw pack size. This is intentionally below LS_PACK_TEXT_MAX:
// parsing and storage keep the larger format bound for older 100-word packs.
// The buffer is allocated after the handshake, so every byte here competes
// with the live TLS session.
#define PACK_STREAM_MAX (LS_GEN_PACK_TEXT_MAX + 512)
#define READ_CHUNK 512
// One SSE event line. DeepSeek emits small delta chunks; a line this long is
// either an error body or a truncated/broken stream.
#define SSE_LINE_MAX 1024

static const char *TAG = "ls_deepseek";

// Detailed failure explanation shown behind "DOWN: DETAIL" on the status
// screen. Kept short enough for the 240 px display and copied into the online
// message before the next attempt overwrites it.
#define DETAIL_CAP 224
static char s_last_error[DETAIL_CAP];

static void set_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

const char *ls_deepseek_last_error(void)
{
    return s_last_error;
}

static bool write_all(esp_http_client_handle_t client, const char *data,
                      size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        int written = esp_http_client_write(client, data + offset,
                                            len - offset);
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

// Largest contiguous block is the number that actually matters on this part:
// the heap can report tens of kilobytes free while no single allocation this
// size can succeed.
static void log_open_failure(esp_http_client_handle_t client, esp_err_t err)
{
    ESP_LOGE(TAG, "HTTP 打开失败: %s, errno=%d, 剩余堆=%u, 最大连续=%u 字节",
             esp_err_to_name(err), esp_http_client_get_errno(client),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

typedef struct {
    esp_http_client_handle_t client;
} http_sink_t;

// Adapter that lets ls_gen_emit_request() write the body straight to the
// socket without a request-sized heap buffer.
static bool http_sink(void *ctx, const char *data, size_t len)
{
    http_sink_t *sink = (http_sink_t *)ctx;
    return write_all(sink->client, data, len);
}

esp_err_t ls_deepseek_generate(const ls_deepseek_cfg_t *cfg,
                               char **pack_out, size_t *pack_len)
{
    if (!cfg || !cfg->api_key || cfg->api_key[0] == '\0' ||
        !pack_out || !pack_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *pack_out = NULL;
    *pack_len = 0;
    s_last_error[0] = '\0';

    const char *model = (cfg->model && cfg->model[0]) ? cfg->model
                                                       : DEFAULT_MODEL;
    uint32_t timeout_ms = cfg->timeout_ms ? cfg->timeout_ms
                                          : LS_GEN_DEFAULT_TIMEOUT_MS;

    esp_err_t err = ESP_FAIL;
    char *prompt = (char *)malloc(PROMPT_CAP);
    size_t prompt_len = 0;
    char *pack = NULL;
    esp_http_client_handle_t client = NULL;

    if (!prompt) {
        set_error("out of memory for prompt (%u free, %u largest)",
                  (unsigned)esp_get_free_heap_size(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        ESP_LOGE(TAG, "分配提示缓冲失败, 剩余堆=%u, 最大连续=%u 字节",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        return ESP_ERR_NO_MEM;
    }
    if (!ls_gen_build_prompt_ex(prompt, PROMPT_CAP, &prompt_len, cfg->exam,
                                cfg->avoid_words, cfg->strict_avoid,
                                cfg->seed, cfg->level)) {
        set_error("prompt does not fit %u bytes (avoid list %u bytes)",
                  (unsigned)PROMPT_CAP,
                  (unsigned)strlen(cfg->avoid_words ? cfg->avoid_words : ""));
        free(prompt);
        return ESP_ERR_INVALID_SIZE;
    }

    // Content-Length is computed with the same escaping rules that emit the
    // body. The body itself is streamed in 256-byte chunks after the socket
    // opens, so no request-sized allocation competes with the TLS handshake.
    size_t body_len = ls_gen_request_size(model, prompt);
    if (body_len == 0) {
        set_error("request body size overflow");
        free(prompt);
        return ESP_ERR_INVALID_SIZE;
    }
    // The pack buffer is allocated only after the TLS handshake completes:
    // holding it during mbedtls_ssl_setup() starves the handshake on the
    // SRAM-only ESP32-C3 and surfaces as ESP_ERR_HTTP_CONNECT
    // (MBEDTLS_ERR_SSL_ALLOC_FAILED).

    esp_http_client_config_t http_cfg = {
        .url = DEEPSEEK_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = (int)timeout_ms,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    client = esp_http_client_init(&http_cfg);
    if (!client) {
        set_error("out of memory for HTTP client");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    (void)esp_http_client_set_header(client, "Content-Type",
                                     "application/json");
    (void)esp_http_client_set_header(client, "Accept", "text/event-stream");
    char auth[8 + LS_API_KEY_MAX];
    int auth_len = snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
    if (auth_len < 0 || (size_t)auth_len >= sizeof(auth)) {
        set_error("API key too long for the auth header");
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    (void)esp_http_client_set_header(client, "Authorization", auth);

    ESP_LOGI(TAG, "打开 HTTPS 前剩余堆=%u, 最大连续=%u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        set_error("HTTPS open failed: %s (errno=%d, free=%u)",
                  esp_err_to_name(err), esp_http_client_get_errno(client),
                  (unsigned)esp_get_free_heap_size());
        log_open_failure(client, err);
        goto cleanup;
    }
    http_sink_t sink = { .client = client };
    if (!ls_gen_emit_request(http_sink, &sink, model, prompt)) {
        set_error("request write failed: errno=%d",
                  esp_http_client_get_errno(client));
        ESP_LOGE(TAG, "HTTPS 请求体写入失败, errno=%d, 剩余堆=%u 字节",
                 esp_http_client_get_errno(client),
                 (unsigned)esp_get_free_heap_size());
        err = ESP_FAIL;
        goto cleanup;
    }
    // The prompt is no longer needed once the body is on the wire.
    free(prompt);
    prompt = NULL;
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    // TLS is up, so the handshake no longer competes for heap. Allocate only
    // the pack-sized buffer. Response bytes are streamed into it and unescaped
    // as they arrive, so the raw JSON body never has to be resident.
    pack = (char *)malloc(PACK_STREAM_MAX);
    if (!pack) {
        set_error("out of memory for pack buffer (%u bytes, free=%u)",
                  (unsigned)PACK_STREAM_MAX,
                  (unsigned)esp_get_free_heap_size());
        ESP_LOGE(TAG, "分配词包缓冲失败(%u 字节), 剩余堆=%u, 最大连续=%u 字节",
                 (unsigned)PACK_STREAM_MAX,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    // Record the post-handshake headroom on the success path too: the next
    // streaming failure needs the numbers from before the first read, not only
    // the leftovers after the heap has been squeezed.
    ESP_LOGI(TAG, "词包缓冲已分配(%u 字节), 剩余堆=%u, 最大连续=%u, DMA最大=%u 字节",
             (unsigned)PACK_STREAM_MAX,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    // DeepSeek streams OpenAI-compatible Server-Sent Events while it generates:
    // each line is "data: {...}" and carries a small delta.content fragment.
    // Consuming them as they arrive keeps the socket active for the whole
    // generation, so a 60-second completion no longer looks like a read
    // timeout. Fragments are JSON-decoded and appended straight into the pack
    // buffer, so the raw response is never resident.
    char chunk[READ_CHUNK];
    char line[SSE_LINE_MAX];
    char preview[161];
    size_t line_len = 0;
    size_t preview_len = 0;
    size_t content_len = 0;
    bool saw_done = false;
    bool truncated = false;
    bool line_overflow = false;
    bool pack_overflow = false;

    for (;;) {
        int read = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read < 0) {
            // A failed read can be a socket error or an allocation failure
            // inside the AES-DMA path; errno alone is 0 for the latter. The
            // TLS error code names the real cause, and the DMA-capable heap
            // numbers show whether the driver ran out of buffers even though
            // other heap regions still had bytes free.
            int tls_error = 0;
            int tls_flags = 0;
            (void)esp_http_client_get_and_clear_last_tls_error(
                client, &tls_error, &tls_flags);
            unsigned dma_free =
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA);
            unsigned dma_largest =
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
            set_error("response read failed: ret=%d errno=%d tls=%d "
                      "(after %u bytes, dma free=%u)",
                      read, esp_http_client_get_errno(client), tls_error,
                      (unsigned)content_len, dma_free);
            ESP_LOGE(TAG, "HTTPS 响应读取失败, ret=%d, errno=%d, TLS错误=%d, "
                     "证书标志=0x%x, 剩余堆=%u, 最大连续=%u, DMA剩余=%u, "
                     "DMA最大=%u 字节",
                     read, esp_http_client_get_errno(client), tls_error,
                     (unsigned)tls_flags,
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_DEFAULT),
                     dma_free, dma_largest);
            err = ESP_FAIL;
            goto cleanup;
        }
        if (read == 0) break;
        if (preview_len < 160) {
            size_t room = 160 - preview_len;
            size_t take = (size_t)read < room ? (size_t)read : room;
            memcpy(preview + preview_len, chunk, take);
            preview_len += take;
        }

        for (int i = 0; i < read; i++) {
            char c = chunk[i];
            if (c != '\n') {
                if (line_len + 1 < sizeof(line)) {
                    line[line_len++] = c;
                } else {
                    // A broken/oversized event line; resync at the next one.
                    line_overflow = true;
                    line_len = 0;
                }
                continue;
            }

            size_t line_used = line_len;
            if (line_used && line[line_used - 1] == '\r') line_used--;
            line[line_used] = '\0';
            line_len = 0;

            if (line_used < 5 || memcmp(line, "data:", 5) != 0) continue;
            const char *payload = line + 5;
            size_t payload_len = line_used - 5;
            while (payload_len &&
                   (*payload == ' ' || *payload == '\t')) {
                payload++;
                payload_len--;
            }
            if (payload_len == 0) continue;
            if (payload_len == 6 && memcmp(payload, "[DONE]", 6) == 0) {
                saw_done = true;
                continue;
            }
            if (strstr(payload, "\"finish_reason\":\"length\"") != NULL ||
                strstr(payload, "\"finish_reason\": \"length\"") != NULL) {
                truncated = true;
            }

            if (content_len >= PACK_STREAM_MAX - 1) {
                pack_overflow = true;
                continue;
            }
            size_t piece_len = 0;
            if (ls_json_extract_string(payload, payload_len, "content",
                                       pack + content_len,
                                       PACK_STREAM_MAX - content_len,
                                       &piece_len)) {
                content_len += piece_len;
            }
            // Events without a content field (role-only deltas, usage) are
            // ignored.
        }
    }
    preview[preview_len] = '\0';
    bool complete = esp_http_client_is_complete_data_received(client);
    // Close and free the TLS session before finalizing so the peak heap never
    // holds TLS state and the full pack at the same time.
    (void)esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;

    if (status != 200) {
        set_error("HTTP %d from DeepSeek: %.96s", status,
                  preview_len ? preview : "(empty response)");
        ESP_LOGE(TAG, "DeepSeek 返回 HTTP %d", status);
        if (preview_len > 0) ESP_LOGW(TAG, "响应片段: %s", preview);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (!complete) {
        set_error("connection closed early after %u content bytes",
                  (unsigned)content_len);
        ESP_LOGE(TAG, "响应未完整接收");
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (line_overflow || pack_overflow) {
        set_error("stream overflow: line=%d pack=%d (limit %u bytes)",
                  (int)line_overflow, (int)pack_overflow,
                  (unsigned)PACK_STREAM_MAX);
        ESP_LOGE(TAG, "SSE 行溢出=%d, 词包溢出=%d (上限 %u 字节)",
                 (int)line_overflow, (int)pack_overflow,
                 (unsigned)PACK_STREAM_MAX);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (truncated) {
        // max_tokens cut the model off. The sanitizer below keeps the complete
        // lines and drops the partial tail, so a 50-odd-word pack still works
        // instead of failing the whole refresh.
        ESP_LOGW(TAG, "模型输出被 max_tokens 截断, 保留完整行");
    }
    if (content_len == 0) {
        set_error("no content in SSE stream (HTTP %d): %.96s", status,
                  preview_len ? preview : "(empty)");
        ESP_LOGE(TAG, "SSE 响应中没有 content");
        if (preview_len > 0) ESP_LOGW(TAG, "响应片段: %s", preview);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (!saw_done) {
        ESP_LOGW(TAG, "SSE 流未以 [DONE] 结束 (content=%u 字节)",
                 (unsigned)content_len);
    }
    if (content_len < PACK_STREAM_MAX) pack[content_len] = '\0';
    if (!ls_gen_finalize_pack(pack, &content_len, PACK_STREAM_MAX)) {
        set_error("no LSPACK pack in reply (%u bytes): %.96s",
                  (unsigned)content_len, pack);
        ESP_LOGE(TAG, "无法从响应中提取有效内容");
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    // IELTS generations sometimes stop a few lines early or append prose.
    // Trim to the complete LESSON/WORD lines and correct the header counts;
    // reject only when too little usable vocabulary remains.
    size_t before = content_len;
    if (!ls_gen_sanitize_pack(pack, &content_len, PACK_STREAM_MAX,
                              LS_WORD_MIN_ACCEPT)) {
        set_error("only %u usable bytes; need %u complete WORD lines: %.96s",
                  (unsigned)before, (unsigned)LS_WORD_MIN_ACCEPT, pack);
        ESP_LOGE(TAG, "词包可用行不足 (%u 字节)", (unsigned)before);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (content_len != before) {
        ESP_LOGW(TAG, "词包已裁剪: %u -> %u 字节", (unsigned)before,
                 (unsigned)content_len);
    }
    *pack_len = content_len;

    // Hand the buffer to the caller; it now holds the finalized pack.
    *pack_out = pack;
    pack = NULL;

    ESP_LOGI(TAG, "生成完成: %u 字节, 剩余堆=%u 字节", (unsigned)*pack_len,
             (unsigned)esp_get_free_heap_size());
    err = ESP_OK;

cleanup:
    if (client) {
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(pack);
    free(prompt);
    return err;
}

esp_err_t ls_deepseek_probe(const ls_deepseek_cfg_t *cfg,
                            uint32_t timeout_ms, int *status_out)
{
    if (status_out) *status_out = 0;
    if (!cfg || !cfg->api_key || cfg->api_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_OK;

    esp_http_client_config_t http_cfg = {
        .url = DEEPSEEK_MODELS_URL,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = (int)(timeout_ms ? timeout_ms : PROBE_TIMEOUT_MS),
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_ERR_NO_MEM;

    char auth[8 + LS_API_KEY_MAX];
    int auth_len = snprintf(auth, sizeof(auth), "Bearer %s", cfg->api_key);
    if (auth_len < 0 || (size_t)auth_len >= sizeof(auth)) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    (void)esp_http_client_set_header(client, "Authorization", auth);

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        log_open_failure(client, err);
        goto cleanup;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status_out) *status_out = status;

    // Drain a bounded amount so the socket can close cleanly; the body is not
    // needed, only the status code.
    char drain[256];
    size_t drained = 0;
    while (drained < PROBE_DRAIN_MAX) {
        int read = esp_http_client_read(client,
                                        drain, sizeof(drain));
        if (read <= 0) break;
        drained += (size_t)read;
    }

    if (status == 200) {
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "连接检查返回 HTTP %d", status);
        err = ESP_ERR_INVALID_RESPONSE;
    }

cleanup:
    if (client) {
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return err;
}
