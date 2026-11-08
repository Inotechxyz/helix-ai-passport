// main/langstudy_sync.c
#include "langstudy_sync.h"

#include "langstudy_azure.h"
#include "langstudy_buddy.h"
#include "langstudy_config.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ls_sync";

#define AZURE_API_VERSION "2021-08-06"
#define AZURE_BLOB_NAME "progress.lsprog"
// Before 2020 the device clock cannot have been set by SNTP; Azure rejects
// stale x-ms-date values, so fail with a clear state error instead.
#define AZURE_MIN_EPOCH 1577836800

// The HTTP client's own buffers are allocated before the TLS handshake, where
// heap is tightest, so keep them small. The response body is streamed into a
// growing buffer only after open() has completed the handshake (the same
// pattern the DeepSeek client uses).
#define HTTP_RX_BUFFER 1024
#define HTTP_TX_BUFFER 512
#define READ_CHUNK 512
#define DOC_INITIAL_SIZE 1024

_Static_assert(LS_SYNC_URL_MAX == LS_AZURE_URL_MAX,
               "sync URL limit must match the Azure URL limit");

static bool write_all(esp_http_client_handle_t client, const char *data,
                      size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        int written = esp_http_client_write(client, data + offset,
                                            (int)(len - offset));
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

// The SAS lives in the query string, so log only the transport error plus heap
// numbers. The largest contiguous block is what decides whether another
// mbedTLS buffer can be allocated.
static void log_open_failure(esp_http_client_handle_t client, esp_err_t err)
{
    ESP_LOGE(TAG, "Azure connect failed: %s, errno=%d, free=%u, largest=%u",
             esp_err_to_name(err), esp_http_client_get_errno(client),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// Connect and send the request headers. This is where the TLS handshake runs,
// so callers must not hold the document buffer yet: mbedTLS alone needs an
// ~8 KiB input buffer, and the old 20 KiB pre-allocation left it with none.
static esp_err_t client_open(const char *url, esp_http_client_method_t method,
                             const char *date, int write_len,
                             uint32_t timeout_ms,
                             esp_http_client_handle_t *out)
{
    *out = NULL;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = (int)(timeout_ms ? timeout_ms
                                       : LS_SYNC_DEFAULT_TIMEOUT_MS),
        .buffer_size = HTTP_RX_BUFFER,
        .buffer_size_tx = HTTP_TX_BUFFER,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_ERR_NO_MEM;

    (void)esp_http_client_set_header(client, "x-ms-version", AZURE_API_VERSION);
    (void)esp_http_client_set_header(client, "x-ms-date", date);
    if (method == HTTP_METHOD_PUT) {
        (void)esp_http_client_set_header(client, "x-ms-blob-type", "BlockBlob");
        (void)esp_http_client_set_header(client, "Content-Type",
                                         "text/plain; charset=utf-8");
    } else {
        (void)esp_http_client_set_header(client, "Accept", "text/plain");
    }

    esp_err_t err = esp_http_client_open(client, write_len);
    if (err != ESP_OK) {
        log_open_failure(client, err);
        esp_http_client_cleanup(client);
        return err;
    }

    *out = client;
    return ESP_OK;
}

// Stream a 200 response into a buffer that grows with the actual document.
// Starting small matters because this runs while the TLS session is live; a
// typical profile document is a few KiB, not the 20 KiB format bound.
static esp_err_t read_document(esp_http_client_handle_t client, char **out,
                               size_t *used)
{
    *out = NULL;
    *used = 0;

    size_t cap = DOC_INITIAL_SIZE;
    char *doc = (char *)malloc(cap + 1);
    if (!doc) return ESP_ERR_NO_MEM;

    char chunk[READ_CHUNK];
    size_t len = 0;
    for (;;) {
        int read = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read < 0) {
            free(doc);
            return ESP_FAIL;
        }
        if (read == 0) break;

        size_t need = len + (size_t)read;
        if (need > LS_PROGRESS_TEXT_MAX) {
            free(doc);
            return ESP_ERR_INVALID_SIZE;
        }
        if (need > cap) {
            size_t next = cap;
            while (next < need) next *= 2;
            if (next > LS_PROGRESS_TEXT_MAX) next = LS_PROGRESS_TEXT_MAX;
            char *grown = (char *)realloc(doc, next + 1);
            if (!grown) {
                free(doc);
                return ESP_ERR_NO_MEM;
            }
            doc = grown;
            cap = next;
        }
        memcpy(doc + len, chunk, (size_t)read);
        len = need;
    }

    if (!esp_http_client_is_complete_data_received(client) || len == 0) {
        free(doc);
        return ESP_ERR_INVALID_SIZE;
    }

    doc[len] = '\0';
    *out = doc;
    *used = len;
    return ESP_OK;
}

// GET profiles/<id>/progress.lsprog. A non-200 status is still ESP_OK: the
// caller distinguishes 404 (first sync) from 401/403 (bad SAS).
static esp_err_t azure_get(const char *url, const char *date,
                           uint32_t timeout_ms, char **doc, size_t *used,
                           int *status)
{
    *status = 0;
    *doc = NULL;
    *used = 0;

    esp_http_client_handle_t client = NULL;
    esp_err_t err = client_open(url, HTTP_METHOD_GET, date, 0, timeout_ms,
                                &client);
    if (err != ESP_OK) return err;

    (void)esp_http_client_fetch_headers(client);
    *status = esp_http_client_get_status_code(client);
    if (*status == 200) {
        err = read_document(client, doc, used);
    }

    (void)esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// PUT the merged document. The body is serialized only after the handshake and
// sized exactly, so the peak appears after mbedTLS has finished connecting.
static esp_err_t azure_put(const char *url, const char *date,
                           const ls_progress_t *merged, uint32_t timeout_ms,
                           int *status)
{
    *status = 0;

    size_t doc_len = 0;
    if (ls_progress_write_size(merged, &doc_len) != LS_OK ||
        doc_len == 0 || doc_len > LS_PROGRESS_TEXT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_handle_t client = NULL;
    esp_err_t err = client_open(url, HTTP_METHOD_PUT, date, (int)doc_len,
                                timeout_ms, &client);
    if (err != ESP_OK) return err;

    char *body = (char *)malloc(doc_len);
    if (!body) {
        err = ESP_ERR_NO_MEM;
    } else {
        size_t written = 0;
        if (ls_progress_write(merged, body, doc_len, &written) != LS_OK ||
            written != doc_len) {
            err = ESP_ERR_INVALID_SIZE;
        } else if (!write_all(client, body, doc_len)) {
            err = ESP_FAIL;
        }
        free(body);
    }

    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        *status = esp_http_client_get_status_code(client);
    }

    (void)esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ls_sync_exchange(const char *container_sas, const char *profile_id,
                           const ls_progress_t *local, ls_progress_t *remote,
                           ls_sync_result_t *result, uint32_t timeout_ms)
{
    if (result) memset(result, 0, sizeof(*result));
    if (!container_sas || !profile_id || !local || !remote) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ls_azure_url_valid(container_sas)) return ESP_ERR_INVALID_ARG;
    if (!ls_profile_valid(profile_id)) return ESP_ERR_INVALID_ARG;

    time_t now = time(NULL);
    if (now < AZURE_MIN_EPOCH) return ESP_ERR_INVALID_STATE;

    char date[40];
    if (!ls_azure_http_date((int64_t)now, date, sizeof(date))) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[LS_AZURE_BLOB_URL_MAX];
    if (!ls_azure_blob_url(container_sas, profile_id, AZURE_BLOB_NAME, url,
                           sizeof(url))) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Azure sync start: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    ls_progress_t *merged = NULL;
    char *doc = NULL;
    size_t used = 0;
    int status = 0;

    esp_err_t err = azure_get(url, date, timeout_ms, &doc, &used, &status);
    if (result) result->http_status = status;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Azure GET failed: %s", esp_err_to_name(err));
        goto out;
    }

    if (status == 200) {
        if (ls_progress_parse(doc, used, remote) != LS_OK) {
            ESP_LOGE(TAG, "Azure progress document is malformed");
            err = ESP_ERR_INVALID_RESPONSE;
            goto out;
        }
        if (result) result->remote_found = true;

        // Drop the response before the PUT handshake so mbedTLS and the
        // merged copy are not competing with it for heap.
        free(doc);
        doc = NULL;

        merged = (ls_progress_t *)malloc(sizeof(*merged));
        if (!merged) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        // Local stays authoritative: ties keep the local record and the
        // counter block follows the side with more completed sessions.
        *merged = *local;
        (void)ls_progress_merge(merged, remote);
    } else if (status == 404) {
        // First sync for this profile. The PUT below creates the blob.
        merged = (ls_progress_t *)malloc(sizeof(*merged));
        if (!merged) {
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        *merged = *local;
    } else if (status == 401 || status == 403) {
        ESP_LOGW(TAG, "Azure rejected the SAS (HTTP %d)", status);
        err = ESP_ERR_INVALID_STATE;
        goto out;
    } else {
        ESP_LOGE(TAG, "Azure GET returned HTTP %d", status);
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }

    int put_status = 0;
    err = azure_put(url, date, merged, timeout_ms, &put_status);
    if (result) result->http_status = put_status;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Azure PUT failed: %s", esp_err_to_name(err));
        goto out;
    }

    if (put_status == 200 || put_status == 201) {
        *remote = *merged;
        if (result && !result->remote_found) result->created = true;
        err = ESP_OK;
    } else if (put_status == 401 || put_status == 403) {
        ESP_LOGW(TAG, "Azure rejected the write (HTTP %d)", put_status);
        err = ESP_ERR_INVALID_STATE;
    } else if (put_status == 404) {
        ESP_LOGE(TAG, "Azure container was not found");
        err = ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGE(TAG, "Azure PUT returned HTTP %d", put_status);
        err = ESP_ERR_INVALID_RESPONSE;
    }

out:
    free(merged);
    free(doc);
    return err;
}
