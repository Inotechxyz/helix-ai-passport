// components/bsp/src/bsp_storage.c
#include "bsp_storage.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "bsp_storage";

esp_err_t bsp_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) return ESP_OK;

    // Erasing here would destroy progress and settings. Report the failure and
    // let the application decide how to recover.
    ESP_LOGE(TAG, "NVS 初始化失败: %s; 未擦除分区", esp_err_to_name(err));
    return err;
}

esp_err_t bsp_storage_open(const char *namespace_name, nvs_handle_t *out)
{
    if (!namespace_name || !out) return ESP_ERR_INVALID_ARG;
    *out = 0;
    return nvs_open(namespace_name, NVS_READWRITE, out);
}

esp_err_t bsp_storage_commit(nvs_handle_t handle)
{
    return nvs_commit(handle);
}

esp_err_t bsp_storage_close(nvs_handle_t handle)
{
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t bsp_storage_get_blob(nvs_handle_t handle, const char *key,
                               void *buf, size_t *out_len)
{
    if (!key || !out_len) return ESP_ERR_INVALID_ARG;
    return nvs_get_blob(handle, key, buf, out_len);
}

esp_err_t bsp_storage_set_blob(nvs_handle_t handle, const char *key,
                               const void *buf, size_t len)
{
    if (!key || (!buf && len != 0)) return ESP_ERR_INVALID_ARG;
    return nvs_set_blob(handle, key, buf, len);
}

esp_err_t bsp_storage_get_u8(nvs_handle_t handle, const char *key, uint8_t *out)
{
    if (!key || !out) return ESP_ERR_INVALID_ARG;
    return nvs_get_u8(handle, key, out);
}

esp_err_t bsp_storage_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    return nvs_set_u8(handle, key, value);
}

esp_err_t bsp_storage_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    if (!key || !out) return ESP_ERR_INVALID_ARG;
    return nvs_get_u32(handle, key, out);
}

esp_err_t bsp_storage_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    return nvs_set_u32(handle, key, value);
}

esp_err_t bsp_storage_get_str(nvs_handle_t handle, const char *key,
                              char *buf, size_t *out_len)
{
    if (!key || !out_len) return ESP_ERR_INVALID_ARG;
    return nvs_get_str(handle, key, buf, out_len);
}

esp_err_t bsp_storage_set_str(nvs_handle_t handle, const char *key,
                              const char *value)
{
    if (!key || !value) return ESP_ERR_INVALID_ARG;
    return nvs_set_str(handle, key, value);
}

esp_err_t bsp_storage_erase(nvs_handle_t handle, const char *key)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_erase_key(handle, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t bsp_storage_erase_all(nvs_handle_t handle)
{
    return nvs_erase_all(handle);
}
