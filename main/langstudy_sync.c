// main/langstudy_sync.c
#include "langstudy_sync.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ls_sync";

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

esp_err_t ls_sync_exchange(const char *url, uint32_t pack_crc,
                           const ls_progress_t *local,
                           ls_progress_t *remote,
                           uint32_t timeout_ms)
{
    if (!url || !local || !remote) return ESP_ERR_INVALID_ARG;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_FAIL;
    char *body = NULL;
    char *response = NULL;
    esp_http_client_handle_t client = NULL;

    body = (char *)malloc(LS_PROGRESS_TEXT_MAX);
    response = (char *)malloc(LS_PROGRESS_TEXT_MAX);
    if (!body || !response) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    size_t body_len = 0;
    if (ls_progress_write(local, body, LS_PROGRESS_TEXT_MAX, &body_len) !=
        LS_OK) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    bool https = (strncmp(url, "https://", 8) == 0);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = https ? esp_crt_bundle_attach : NULL,
        .timeout_ms = (int)(timeout_ms ? timeout_ms
                                       : LS_SYNC_DEFAULT_TIMEOUT_MS),
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    client = esp_http_client_init(&http_cfg);
    if (!client) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    (void)esp_http_client_set_header(client, "Content-Type", "text/plain");
    char crc_header[9];
    (void)snprintf(crc_header, sizeof(crc_header), "%08lX",
                   (unsigned long)pack_crc);
    (void)esp_http_client_set_header(client, "X-Pack-CRC", crc_header);

    err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "同步 HTTP 打开失败: %s", esp_err_to_name(err));
        goto cleanup;
    }
    if (!write_all(client, body, body_len)) {
        err = ESP_FAIL;
        goto cleanup;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    size_t used = 0;
    for (;;) {
        if (used >= LS_PROGRESS_TEXT_MAX) break;
        int read = esp_http_client_read(client, response + used,
                                        LS_PROGRESS_TEXT_MAX - used);
        if (read < 0) {
            err = ESP_FAIL;
            goto cleanup;
        }
        if (read == 0) break;
        used += (size_t)read;
    }
    bool complete = esp_http_client_is_complete_data_received(client);
    (void)esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;

    if (status != 200) {
        ESP_LOGE(TAG, "同步服务器返回 HTTP %d", status);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (!complete || used == 0) {
        ESP_LOGE(TAG, "同步响应不完整");
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (ls_progress_parse(response, used, remote) != LS_OK) {
        ESP_LOGE(TAG, "同步响应格式错误");
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    err = ESP_OK;

cleanup:
    if (client) {
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(response);
    free(body);
    return err;
}
