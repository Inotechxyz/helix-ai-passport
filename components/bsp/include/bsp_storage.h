// components/bsp/include/bsp_storage.h
// NVS-backed key/value storage shared by applications.
//
// Initialization never erases user data: a failed NVS init is reported to the
// caller instead of silently formatting the partition. Applications own their
// namespace/keys and must commit explicitly so a group of writes becomes
// durable together.
#pragma once

#include "esp_err.h"
#include "nvs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Initialize the default NVS partition. Idempotent. On failure the partition is
// left untouched so previously saved application data survives.
esp_err_t bsp_storage_init(void);

// Open a namespace. The returned handle must be closed with bsp_storage_close().
// ESP_ERR_NVS_NOT_FOUND means the namespace does not exist yet; callers that
// only read may treat it as empty.
esp_err_t bsp_storage_open(const char *namespace_name, nvs_handle_t *out);

// Flush pending writes to flash.
esp_err_t bsp_storage_commit(nvs_handle_t handle);

// Close the handle and release its resources. Pending writes are not flushed;
// call bsp_storage_commit() first when durability is required.
esp_err_t bsp_storage_close(nvs_handle_t handle);

// Blob helpers. For reads, *out_len is the buffer capacity on input and the
// stored length on output. Pass buf = NULL to query the required length.
// ESP_ERR_NVS_NOT_FOUND means the key is absent.
esp_err_t bsp_storage_get_blob(nvs_handle_t handle, const char *key,
                               void *buf, size_t *out_len);
esp_err_t bsp_storage_set_blob(nvs_handle_t handle, const char *key,
                               const void *buf, size_t len);

// Scalar helpers.
esp_err_t bsp_storage_get_u8(nvs_handle_t handle, const char *key, uint8_t *out);
esp_err_t bsp_storage_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t bsp_storage_get_u32(nvs_handle_t handle, const char *key, uint32_t *out);
esp_err_t bsp_storage_set_u32(nvs_handle_t handle, const char *key, uint32_t value);

// String helpers. For reads, *out_len is the buffer capacity on input and the
// stored length on output (including the terminating NUL).
esp_err_t bsp_storage_get_str(nvs_handle_t handle, const char *key,
                              char *buf, size_t *out_len);
esp_err_t bsp_storage_set_str(nvs_handle_t handle, const char *key,
                              const char *value);

// Remove a single key or every key in the open namespace. A missing key is not
// an error for bsp_storage_erase().
esp_err_t bsp_storage_erase(nvs_handle_t handle, const char *key);
esp_err_t bsp_storage_erase_all(nvs_handle_t handle);
