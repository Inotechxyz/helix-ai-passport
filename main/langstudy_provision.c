// main/langstudy_provision.c
#include "langstudy_provision.h"

#include "langstudy_net.h"
#include "langstudy_setup_form.h"
#include "langstudy_setup_page.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROV_TASK_STACK 6144
#define PROV_TASK_PRIO 4
#define PROV_QUEUE_DEPTH 2
#define PROV_FORM_MAX 2048
// The first association after a boot also pays for the targeted scan and, on
// some routers, DHCP; 15 s was short enough that a slow-but-working AP looked
// like a failure while the SoftAP was still serving the phone.
#define PROV_CONNECT_TIMEOUT_MS 30000

typedef enum {
    PROV_IDLE = 0,
    PROV_CONNECTING,
    PROV_OK,
    PROV_FAIL,
} prov_state_t;

typedef struct {
    char ssid[33];
    char pass[64];
    char api[LS_API_KEY_MAX + 1];
    char sync[LS_SYNC_URL_MAX + 1];
    bool clear_api;
} prov_request_t;

static const char *TAG = "ls_prov";

static httpd_handle_t s_server;
static TaskHandle_t s_worker;
static QueueHandle_t s_requests;
static SemaphoreHandle_t s_status_mutex;
static volatile bool s_running;
static prov_state_t s_state = PROV_IDLE;
static char s_message[96] = "Ready";
static ls_config_t *s_config;
static char s_ap_ssid[33];
static char s_ap_password[17];

static const char *state_name(prov_state_t state)
{
    switch (state) {
    case PROV_CONNECTING: return "connecting";
    case PROV_OK: return "ok";
    case PROV_FAIL: return "fail";
    default: return "idle";
    }
}

static void set_status(prov_state_t state, const char *message)
{
    if (!s_status_mutex) return;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    s_state = state;
    if (message) {
        snprintf(s_message, sizeof(s_message), "%s", message);
    } else {
        s_message[0] = '\0';
    }
    xSemaphoreGive(s_status_mutex);
}

static void get_status(prov_state_t *state, char *message, size_t cap)
{
    if (!s_status_mutex || !state || !message || cap == 0) return;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    *state = s_state;
    snprintf(message, cap, "%s", s_message);
    xSemaphoreGive(s_status_mutex);
}

static int recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) return -1;
    size_t total = 0;
    while (total < req->content_len) {
        int read = httpd_req_recv(req, buf + total, req->content_len - total);
        if (read == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (read <= 0) return -1;
        total += (size_t)read;
    }
    buf[total] = '\0';
    return (int)total;
}

static bool form_field(const char *body, int len, const char *key,
                       char *out, size_t cap)
{
    size_t out_len = 0;
    if (!ls_form_get(body, (size_t)len, key, out, cap, &out_len)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static esp_err_t send_redirect(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t get_root(httpd_req_t *req)
{
    char *page = (char *)malloc(LS_SETUP_PAGE_MAX);
    if (!page) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }
    size_t len = ls_setup_page_render(page, LS_SETUP_PAGE_MAX, s_config,
                                      s_ap_ssid, "http://192.168.4.1");
    if (len == 0) {
        free(page);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Page too large");
    }
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, page, len);
    free(page);
    return err;
}

static esp_err_t get_status_handler(httpd_req_t *req)
{
    prov_state_t state = PROV_IDLE;
    char message[96];
    get_status(&state, message, sizeof(message));

    char body[192];
    (void)snprintf(body, sizeof(body),
                   "{\"state\":\"%s\",\"message\":\"%s\"}",
                   state_name(state), message);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t post_save_handler(httpd_req_t *req)
{
    char *body = (char *)malloc(PROV_FORM_MAX);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }
    int len = recv_body(req, body, PROV_FORM_MAX);
    if (len < 0) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form");
    }

    prov_request_t request;
    memset(&request, 0, sizeof(request));
    (void)form_field(body, len, "ssid", request.ssid, sizeof(request.ssid));
    (void)form_field(body, len, "pass", request.pass, sizeof(request.pass));
    (void)form_field(body, len, "api", request.api, sizeof(request.api));
    (void)form_field(body, len, "sync", request.sync, sizeof(request.sync));
    char flag[2];
    request.clear_api = form_field(body, len, "clear_api", flag, sizeof(flag));
    free(body);

    if (!ls_setup_validate_ssid(request.ssid) ||
        !ls_setup_validate_password(request.pass) ||
        (!request.clear_api && !ls_setup_validate_api_key(request.api)) ||
        !ls_setup_validate_server_url(request.sync)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Invalid field");
    }

    // Blank password + unchanged SSID means "keep the stored password"; blank
    // password for a new SSID means an open network.
    if (request.pass[0] == '\0' && request.ssid[0] != '\0' &&
        strcmp(request.ssid, s_config->wifi_ssid) == 0) {
        snprintf(request.pass, sizeof(request.pass), "%s",
                 s_config->wifi_pass);
    }

    if (!s_requests || xQueueSend(s_requests, &request, 0) != pdTRUE) {
        // esp_http_server has no 503 entry in httpd_err_code_t; set the status
        // line explicitly instead of misreporting the condition as a 500.
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Busy, try again", HTTPD_RESP_USE_STRLEN);
    }
    return send_redirect(req, "/");
}

static esp_err_t post_clear_handler(httpd_req_t *req)
{
    esp_err_t err = ls_config_clear_network();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Clear failed");
    }
    s_config->wifi_ssid[0] = '\0';
    s_config->wifi_pass[0] = '\0';
    set_status(PROV_OK, "Saved Wi-Fi network cleared");
    return send_redirect(req, "/");
}

// Turn a failed STA attempt into something the phone page can act on. The
// driver's reason code is the useful part: a timeout alone cannot distinguish a
// wrong password from a 5 GHz-only network or an out-of-range AP.
static void report_wifi_failure(esp_err_t err)
{
    uint8_t reason = ls_net_last_disconnect_reason();
    if (err == ESP_ERR_NOT_FOUND || reason == WIFI_REASON_NO_AP_FOUND) {
        set_status(PROV_FAIL,
                   "Wi-Fi not found (use a 2.4 GHz network) - nothing saved");
        return;
    }
    if (reason == WIFI_REASON_AUTH_FAIL ||
        reason == WIFI_REASON_AUTH_EXPIRE ||
        reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
        set_status(PROV_FAIL, "Wrong Wi-Fi password - nothing saved");
        return;
    }
    char message[96];
    snprintf(message, sizeof(message), "Wi-Fi failed: %s (%u) - nothing saved",
             esp_err_to_name(err), reason);
    set_status(PROV_FAIL, message);
}

static void provision_worker(void *arg)
{
    (void)arg;
    prov_request_t request;
    for (;;) {
        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bool wifi_changed = request.ssid[0] != '\0';
        if (wifi_changed) {
            set_status(PROV_CONNECTING, "Testing Wi-Fi connection...");
            esp_err_t err = ls_net_sta_connect(request.ssid, request.pass,
                                               PROV_CONNECT_TIMEOUT_MS);
            if (err != ESP_OK) {
                report_wifi_failure(err);
                continue;
            }
        }

        ls_config_t updated = *s_config;
        if (wifi_changed) {
            snprintf(updated.wifi_ssid, sizeof(updated.wifi_ssid), "%s",
                     request.ssid);
            snprintf(updated.wifi_pass, sizeof(updated.wifi_pass), "%s",
                     request.pass);
        }
        if (request.clear_api) {
            updated.api_key[0] = '\0';
        } else if (request.api[0] != '\0') {
            snprintf(updated.api_key, sizeof(updated.api_key), "%s",
                     request.api);
        }
        snprintf(updated.sync_url, sizeof(updated.sync_url), "%s",
                 request.sync);

        esp_err_t err = ls_config_save_all(&updated);
        if (err != ESP_OK) {
            char message[96];
            snprintf(message, sizeof(message), "NVS save failed: %s",
                     esp_err_to_name(err));
            set_status(PROV_FAIL, message);
            continue;
        }
        *s_config = updated;
        set_status(PROV_OK, "Saved. You can close this page.");
        ESP_LOGI(TAG, "设置已保存");
    }
}

esp_err_t ls_provision_start(ls_config_t *config, ls_provision_info_t *info)
{
    if (!config || !info) return ESP_ERR_INVALID_ARG;
    if (s_running) return ESP_ERR_INVALID_STATE;

    s_config = config;

    uint8_t mac[6] = { 0 };
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) return err;

    snprintf(info->ap_ssid, sizeof(info->ap_ssid), "LangStudy-%02X%02X",
             mac[4], mac[5]);
    snprintf(info->ap_password, sizeof(info->ap_password), "learn%02X%02X",
             mac[4], mac[5]);
    snprintf(info->url, sizeof(info->url), "http://192.168.4.1");
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", info->ap_ssid);
    snprintf(s_ap_password, sizeof(s_ap_password), "%s", info->ap_password);

    s_status_mutex = xSemaphoreCreateMutex();
    s_requests = xQueueCreate(PROV_QUEUE_DEPTH, sizeof(prov_request_t));
    if (!s_status_mutex || !s_requests) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    set_status(PROV_IDLE, "Ready. Saving tests Wi-Fi before it is stored.");

    err = ls_net_ap_start(info->ap_ssid, info->ap_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP 启动失败: %s", esp_err_to_name(err));
        goto fail;
    }

    if (xTaskCreate(provision_worker, "ls_prov", PROV_TASK_STACK, NULL,
                    PROV_TASK_PRIO, &s_worker) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 6;
    http_cfg.lru_purge_enable = true;
    http_cfg.stack_size = 6144;
    if (httpd_start(&s_server, &http_cfg) != ESP_OK) {
        err = ESP_FAIL;
        goto fail;
    }

    const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = get_root,
    };
    const httpd_uri_t status = {
        .uri = "/status", .method = HTTP_GET, .handler = get_status_handler,
    };
    const httpd_uri_t save = {
        .uri = "/save", .method = HTTP_POST, .handler = post_save_handler,
    };
    const httpd_uri_t clear = {
        .uri = "/clear", .method = HTTP_POST, .handler = post_clear_handler,
    };
    if (httpd_register_uri_handler(s_server, &root) != ESP_OK ||
        httpd_register_uri_handler(s_server, &status) != ESP_OK ||
        httpd_register_uri_handler(s_server, &save) != ESP_OK ||
        httpd_register_uri_handler(s_server, &clear) != ESP_OK) {
        err = ESP_FAIL;
        goto fail;
    }

    s_running = true;
    // Never log the AP password: the device log may be captured or shared and
    // the password is shown to the user on the setup page instead.
    ESP_LOGI(TAG, "配网已启动: AP %s, 页面 %s", info->ap_ssid, info->url);
    return ESP_OK;

fail:
    ls_provision_stop();
    return err;
}

void ls_provision_stop(void)
{
    s_running = false;
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_worker) {
        vTaskDelete(s_worker);
        s_worker = NULL;
    }
    if (s_requests) {
        vQueueDelete(s_requests);
        s_requests = NULL;
    }
    // Publish the final status while the mutex is still alive; set_status() is a
    // no-op once the mutex has been deleted.
    set_status(PROV_IDLE, "Stopped");
    if (s_status_mutex) {
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = NULL;
    }
    ls_net_shutdown();
    s_config = NULL;
}

bool ls_provision_active(void)
{
    return s_running;
}
