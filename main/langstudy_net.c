// main/langstudy_net.c
#include "langstudy_net.h"

#include "bsp_storage.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <string.h>

#define NET_BIT_GOT_IP BIT0
#define NET_BIT_FAILED BIT1
#define NET_CONNECT_RETRIES 2
#define NET_SNTP_SERVER "pool.ntp.org"

static const char *TAG = "ls_net";

static bool s_netif_ready;
static bool s_event_loop_ready;
static bool s_wifi_initialized;
static bool s_wifi_started;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static EventGroupHandle_t s_events;
static volatile bool s_sta_connected;
static volatile bool s_sta_started;
static volatile ls_net_state_t s_state = LS_NET_IDLE;
static volatile uint8_t s_disconnect_reason;
static int s_retries_left;
static char s_ssid[33];
static char s_password[64];

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void teardown(void)
{
    // Stop the driver before the disconnect event can schedule a retry.
    s_state = LS_NET_IDLE;
    s_retries_left = 0;
    if (s_wifi_started) {
        (void)esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        if (s_wifi_handler) {
            (void)esp_event_handler_instance_unregister(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        s_wifi_handler);
            s_wifi_handler = NULL;
        }
        if (s_ip_handler) {
            (void)esp_event_handler_instance_unregister(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        s_ip_handler);
            s_ip_handler = NULL;
        }
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_sta_connected = false;
    s_sta_started = false;
    s_disconnect_reason = 0;
    s_retries_left = 0;
    s_ssid[0] = '\0';
    s_password[0] = '\0';
    s_state = LS_NET_IDLE;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            s_sta_started = true;
            if (s_ssid[0] != '\0' && s_state == LS_NET_STA_CONNECTING) {
                (void)esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_sta_connected = false;
            if (event_data) {
                const wifi_event_sta_disconnected_t *event =
                    (const wifi_event_sta_disconnected_t *)event_data;
                s_disconnect_reason = event->reason;
            }
            bool trying = (s_state == LS_NET_STA_CONNECTING ||
                           s_state == LS_NET_STA_CONNECTED);
            if (trying && s_retries_left > 0) {
                s_retries_left--;
                s_state = LS_NET_STA_CONNECTING;
                ESP_LOGW(TAG, "STA 断开,重试连接(剩 %d 次)", s_retries_left);
                (void)esp_wifi_connect();
            } else if (trying) {
                s_state = LS_NET_FAILED;
                if (s_events) xEventGroupSetBits(s_events, NET_BIT_FAILED);
            }
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_sta_connected = true;
        s_disconnect_reason = 0;
        s_state = LS_NET_STA_CONNECTED;
        if (s_events) xEventGroupSetBits(s_events, NET_BIT_GOT_IP);
    }
}

esp_err_t ls_net_prepare(void)
{
    // Wi-Fi driver state uses NVS; initialize it once through the BSP wrapper.
    esp_err_t err = bsp_storage_init();
    if (err != ESP_OK) return err;

    if (!s_events) {
        s_events = xEventGroupCreate();
        if (!s_events) return ESP_ERR_NO_MEM;
    }
    if (!s_netif_ready) {
        err = esp_netif_init();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_netif_ready = true;
        } else {
            return err;
        }
    }
    if (!s_event_loop_ready) {
        err = esp_event_loop_create_default();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_event_loop_ready = true;
        } else {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t bring_up(wifi_mode_t mode)
{
    if (s_wifi_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ls_net_prepare();
    if (err != ESP_OK) return err;

    if ((mode & WIFI_MODE_STA) != 0) {
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
        s_sta_netif = esp_netif_new(&cfg);
        if (!s_sta_netif) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        err = esp_netif_attach_wifi_station(s_sta_netif);
        if (err != ESP_OK) goto fail;
    }
    if ((mode & WIFI_MODE_AP) != 0) {
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_AP();
        s_ap_netif = esp_netif_new(&cfg);
        if (!s_ap_netif) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        err = esp_netif_attach_wifi_ap(s_ap_netif);
        if (err != ESP_OK) goto fail;
    }

    if (mode == WIFI_MODE_APSTA) {
        // ESP-IDF has no combined AP+STA helper; register both default handler
        // sets before esp_wifi_init().
        err = esp_wifi_set_default_wifi_ap_handlers();
        if (err == ESP_OK) {
            err = esp_wifi_set_default_wifi_sta_handlers();
        }
    } else if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_default_wifi_ap_handlers();
    } else {
        err = esp_wifi_set_default_wifi_sta_handlers();
    }
    if (err != ESP_OK) goto fail;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) goto fail;
    s_wifi_initialized = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL,
                                              &s_wifi_handler);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL,
                                              &s_ip_handler);
    if (err != ESP_OK) goto fail;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;
    if ((mode & WIFI_MODE_STA) != 0) {
        // Online work runs one TLS request at a time and then tears the radio
        // down; modem sleep only adds latency spikes that can look like HTTP
        // read timeouts in the middle of a generation.
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "关闭 Wi-Fi 省电失败: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
    teardown();
    return err;
}

esp_err_t ls_net_ap_start(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_wifi_initialized) return ESP_ERR_INVALID_STATE;

    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) return ESP_ERR_INVALID_ARG;
    size_t pass_len = password ? strlen(password) : 0;
    if (pass_len > 63) return ESP_ERR_INVALID_ARG;
    if (pass_len > 0 && pass_len < 8) return ESP_ERR_INVALID_ARG;

    esp_err_t err = bring_up(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    memcpy(ap_cfg.ap.ssid, ssid, ssid_len);
    ap_cfg.ap.ssid_len = (uint8_t)ssid_len;
    if (pass_len > 0) {
        memcpy(ap_cfg.ap.password, password, pass_len);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_cfg.ap.max_connection = 2;
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP 配置失败: %s", esp_err_to_name(err));
        teardown();
        return err;
    }
    s_state = LS_NET_AP;
    ESP_LOGI(TAG, "SoftAP 已启动: %s", ssid);
    return ESP_OK;
}

// Scan only for the requested SSID and return its channel/security. A targeted
// scan finishes quickly and lets the following connect stay on one channel;
// without it the driver scans all channels, which is slow and unreliable while
// the provisioning SoftAP is also serving the phone.
#define NET_SCAN_RECORDS 8

static esp_err_t scan_target_ap(const char *ssid, wifi_ap_record_t *out)
{
    wifi_scan_config_t scan_cfg;
    memset(&scan_cfg, 0, sizeof(scan_cfg));
    scan_cfg.ssid = (uint8_t *)ssid;
    scan_cfg.show_hidden = true;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA 扫描启动失败: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t count = NET_SCAN_RECORDS;
    wifi_ap_record_t records[NET_SCAN_RECORDS];
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA 扫描结果读取失败: %s", esp_err_to_name(err));
        return err;
    }

    for (uint16_t i = 0; i < count; i++) {
        if (strncmp((const char *)records[i].ssid, ssid,
                    sizeof(records[i].ssid)) == 0) {
            *out = records[i];
            ESP_LOGI(TAG, "STA 目标 AP: %s ch=%u rssi=%d auth=%d", ssid,
                     records[i].primary, records[i].rssi, records[i].authmode);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "STA 扫描未找到目标 AP: %s", ssid);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ls_net_sta_connect(const char *ssid, const char *password,
                             uint32_t timeout_ms)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    size_t pass_len = password ? strlen(password) : 0;
    if (pass_len > 63 || (pass_len > 0 && pass_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    if (!s_wifi_initialized) {
        err = bring_up(WIFI_MODE_STA);
        if (err != ESP_OK) return err;
    }

    // esp_wifi_connect() must follow WIFI_EVENT_STA_START. It has usually been
    // processed long before the user submits the setup form, but wait briefly
    // so a fast submit cannot race the driver start.
    for (int i = 0; i < 50 && !s_sta_started; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_sta_started) {
        s_state = LS_NET_FAILED;
        return ESP_ERR_INVALID_STATE;
    }

    // Re-saving the same network while already connected is a success, not a
    // failure; reconnecting with different credentials needs a disconnect.
    if (s_sta_connected) {
        if (strcmp(s_ssid, ssid) == 0 &&
            strcmp(s_password, password ? password : "") == 0) {
            ESP_LOGI(TAG, "STA 已连接: %s", ssid);
            return ESP_OK;
        }
        s_state = LS_NET_IDLE;
        s_retries_left = 0;
        (void)esp_wifi_disconnect();
        for (int i = 0; i < 40 && s_sta_connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    wifi_ap_record_t target;
    memset(&target, 0, sizeof(target));
    esp_err_t scan_err = scan_target_ap(ssid, &target);
    if (scan_err == ESP_ERR_NOT_FOUND) {
        s_state = LS_NET_FAILED;
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    memcpy(sta_cfg.sta.ssid, ssid, strlen(ssid));
    if (pass_len > 0) memcpy(sta_cfg.sta.password, password, pass_len);
    sta_cfg.sta.threshold.authmode =
        (pass_len > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
    if (scan_err == ESP_OK) {
        // Connecting on the scanned channel avoids a second all-channel scan
        // and the long channel-switch window that made the phone's page time
        // out during the connection test.
        sta_cfg.sta.channel = target.primary;
    }
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) return err;

    copy_text(s_ssid, sizeof(s_ssid), ssid);
    copy_text(s_password, sizeof(s_password), password ? password : "");
    s_retries_left = NET_CONNECT_RETRIES;
    s_disconnect_reason = 0;
    s_state = LS_NET_STA_CONNECTING;
    if (s_events) {
        xEventGroupClearBits(s_events, NET_BIT_GOT_IP | NET_BIT_FAILED);
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_state = LS_NET_FAILED;
        ESP_LOGW(TAG, "STA 连接调用失败: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = 0;
    if (s_events) {
        bits = xEventGroupWaitBits(s_events, NET_BIT_GOT_IP | NET_BIT_FAILED,
                                   pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(timeout_ms));
    }
    s_retries_left = 0;

    if ((bits & NET_BIT_GOT_IP) != 0 && s_sta_connected) {
        ESP_LOGI(TAG, "STA 已连接: %s", ssid);
        return ESP_OK;
    }
    s_state = LS_NET_FAILED;
    ESP_LOGW(TAG, "STA 连接超时或失败: %s (reason=%u)", ssid,
             s_disconnect_reason);
    return ESP_ERR_TIMEOUT;
}

bool ls_net_sta_connected(void)
{
    return s_sta_connected;
}

uint8_t ls_net_last_disconnect_reason(void)
{
    return s_disconnect_reason;
}

esp_err_t ls_net_sync_time(uint32_t timeout_ms)
{
    if (!s_sta_connected) return ESP_ERR_INVALID_STATE;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NET_SNTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 启动失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    esp_netif_sntp_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 校时失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ls_net_shutdown(void)
{
    teardown();
    return ESP_OK;
}

ls_net_state_t ls_net_state(void)
{
    return s_state;
}
