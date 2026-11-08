// main/langstudy_store.c
#include "langstudy_store.h"

#include "bsp_storage.h"
#include "langstudy_builtin.h"
#include "langstudy_content.h"
#include "langstudy_progress.h"

#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

#define LS_NVS_NAMESPACE "langstudy"
#define KEY_PACK_A "pack_a"
#define KEY_PACK_B "pack_b"
#define KEY_PACK_IDX "pack_idx"
#define KEY_PACK_GEN "pack_gen"
#define KEY_PROGRESS "progress"
#define KEY_AVOID_HISTORY "avoid_hist"
#define KEY_PROFILE_ID "prof_id"
#define KEY_BUDDY "buddy"

static const char *TAG = "ls_store";

static bool s_ready;

esp_err_t ls_store_init(void)
{
    esp_err_t err = bsp_storage_init();
    s_ready = (err == ESP_OK);
    if (!s_ready) {
        // Never erase automatically: that would throw away the user's progress.
        ESP_LOGE(TAG, "NVS 不可用(%s),本次运行使用内置内容且进度不落盘",
                 esp_err_to_name(err));
    }
    return err;
}

bool ls_store_ready(void)
{
    return s_ready;
}

static bool read_slot(nvs_handle_t handle, const char *key, ls_pack_t *out)
{
    size_t len = 0;
    esp_err_t err = bsp_storage_get_blob(handle, key, NULL, &len);
    if (err != ESP_OK || len == 0 || len > LS_PACK_TEXT_MAX) return false;

    char *buf = (char *)malloc(len);
    if (!buf) return false;
    err = bsp_storage_get_blob(handle, key, buf, &len);
    bool ok = (err == ESP_OK) && (ls_pack_parse(buf, len, out) == LS_OK);
    free(buf);
    return ok;
}

bool ls_store_load_pack(ls_pack_t *out, ls_store_info_t *info)
{
    if (!out || !info) return false;
    memset(info, 0, sizeof(*info));
    info->slot = 0xFF;

    if (s_ready) {
        nvs_handle_t handle;
        if (bsp_storage_open(LS_NVS_NAMESPACE, &handle) == ESP_OK) {
            uint8_t index = 0;
            uint32_t generation = 0;
            (void)bsp_storage_get_u8(handle, KEY_PACK_IDX, &index);
            (void)bsp_storage_get_u32(handle, KEY_PACK_GEN, &generation);

            const char *first = (index == 1) ? KEY_PACK_B : KEY_PACK_A;
            const char *second = (index == 1) ? KEY_PACK_A : KEY_PACK_B;
            uint8_t first_slot = (index == 1) ? 1 : 0;

            if (read_slot(handle, first, out)) {
                info->slot = first_slot;
                info->generation = generation;
                info->from_storage = true;
                bsp_storage_close(handle);
                return true;
            }
            if (read_slot(handle, second, out)) {
                ESP_LOGW(TAG, "主内容槽损坏,改用备用槽");
                info->slot = (uint8_t)(first_slot ^ 1u);
                info->generation = generation;
                info->from_storage = true;
                bsp_storage_close(handle);
                return true;
            }
            bsp_storage_close(handle);
        } else {
            ESP_LOGW(TAG, "无法打开内容命名空间,使用内置内容");
        }
    }

    if (!ls_builtin_load(out)) {
        ESP_LOGE(TAG, "内置内容解析失败");
        return false;
    }
    info->slot = 0xFF;
    info->generation = 0;
    info->from_storage = false;
    return true;
}

// Store text that the caller has already validated. Callers holding a parsed
// and validated model use this directly so the pack is not parsed twice.
static esp_err_t save_text_unchecked(const char *text, size_t len,
                                     uint32_t *out_generation)
{
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;

    uint8_t active = 0;
    uint32_t generation = 0;
    (void)bsp_storage_get_u8(handle, KEY_PACK_IDX, &active);
    (void)bsp_storage_get_u32(handle, KEY_PACK_GEN, &generation);

    const char *slot_key = (active == 0) ? KEY_PACK_B : KEY_PACK_A;
    uint8_t next_slot = (uint8_t)((active == 0) ? 1 : 0);

    err = bsp_storage_set_blob(handle, slot_key, text, len);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    if (err == ESP_OK) {
        err = bsp_storage_set_u8(handle, KEY_PACK_IDX, next_slot);
        if (err == ESP_OK) {
            err = bsp_storage_set_u32(handle, KEY_PACK_GEN, generation + 1);
        }
        if (err == ESP_OK) err = bsp_storage_commit(handle);
    }
    bsp_storage_close(handle);

    if (err == ESP_OK && out_generation) *out_generation = generation + 1;
    return err;
}

esp_err_t ls_store_save_pack(const char *text, size_t len,
                             uint32_t *out_generation)
{
    if (out_generation) *out_generation = 0;
    if (!text || len == 0 || len > LS_PACK_TEXT_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    ls_pack_t *check = (ls_pack_t *)malloc(sizeof(*check));
    if (!check) return ESP_ERR_NO_MEM;
    bool ok = ls_pack_parse(text, len, check) == LS_OK;
    free(check);
    if (!ok) return ESP_ERR_INVALID_ARG;

    return save_text_unchecked(text, len, out_generation);
}

esp_err_t ls_store_save_pack_model(const ls_pack_t *pack,
                                   uint32_t *out_generation)
{
    if (out_generation) *out_generation = 0;
    if (!pack || !ls_pack_validate(pack)) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    // Allocate only what this pack serializes to; the format cap is ~20 KiB
    // and the device has no PSRAM.
    size_t cap = ls_pack_text_size(pack);
    if (cap == 0) return ESP_ERR_INVALID_ARG;
    char *text = (char *)malloc(cap);
    if (!text) return ESP_ERR_NO_MEM;
    size_t len = 0;
    ls_status_t status = ls_pack_write(pack, text, cap, &len);
    esp_err_t err = (status == LS_OK)
                        ? save_text_unchecked(text, len, out_generation)
                        : ESP_ERR_INVALID_ARG;
    free(text);
    return err;
}

esp_err_t ls_store_clear_pack(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = bsp_storage_erase(handle, KEY_PACK_A);
    if (err == ESP_OK) err = bsp_storage_erase(handle, KEY_PACK_B);
    if (err == ESP_OK) err = bsp_storage_erase(handle, KEY_PACK_IDX);
    if (err == ESP_OK) err = bsp_storage_erase(handle, KEY_PACK_GEN);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

bool ls_store_load_progress(ls_progress_t *out)
{
    if (!out) return false;
    ls_progress_init(out);
    if (!s_ready) return false;

    nvs_handle_t handle;
    if (bsp_storage_open(LS_NVS_NAMESPACE, &handle) != ESP_OK) return false;

    size_t len = 0;
    esp_err_t err = bsp_storage_get_blob(handle, KEY_PROGRESS, NULL, &len);
    if (err != ESP_OK || len == 0 || len > LS_PROGRESS_TEXT_MAX) {
        bsp_storage_close(handle);
        return false;
    }

    char *buf = (char *)malloc(len);
    if (!buf) {
        bsp_storage_close(handle);
        return false;
    }
    err = bsp_storage_get_blob(handle, KEY_PROGRESS, buf, &len);
    bool ok = (err == ESP_OK) && (ls_progress_parse(buf, len, out) == LS_OK);
    free(buf);
    bsp_storage_close(handle);

    if (!ok) {
        ESP_LOGW(TAG, "进度数据损坏,重新开始(旧数据保留在 NVS 中以便排查)");
        ls_progress_init(out);
    }
    return ok;
}

esp_err_t ls_store_save_progress(const ls_progress_t *progress)
{
    if (!progress) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char *buf = (char *)malloc(LS_PROGRESS_TEXT_MAX);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t len = 0;
    ls_status_t status = ls_progress_write(progress, buf, LS_PROGRESS_TEXT_MAX,
                                           &len);
    if (status != LS_OK) {
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err == ESP_OK) {
        err = bsp_storage_set_blob(handle, KEY_PROGRESS, buf, len);
        if (err == ESP_OK) err = bsp_storage_commit(handle);
        bsp_storage_close(handle);
    }
    free(buf);
    return err;
}

esp_err_t ls_store_clear_progress(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = bsp_storage_erase(handle, KEY_PROGRESS);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

bool ls_store_load_avoid_history(char *out, size_t cap)
{
    if (!out || cap < 2) return false;
    out[0] = '\0';
    if (!s_ready) return false;

    nvs_handle_t handle;
    if (bsp_storage_open(LS_NVS_NAMESPACE, &handle) != ESP_OK) return false;

    // Read straight into the caller's buffer: NVS hands out at most cap - 1
    // bytes, so the terminating NUL always fits.
    size_t len = cap - 1;
    esp_err_t err = bsp_storage_get_blob(handle, KEY_AVOID_HISTORY, out, &len);
    bsp_storage_close(handle);
    if (err != ESP_OK || len == 0 || len > cap - 1) {
        out[0] = '\0';
        return false;
    }

    // Keep the list usable even if a future format change or a partial write
    // left unexpected bytes in the blob. Only printable ASCII outside '|' is a
    // legal word character.
    size_t used = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c >= 0x20 && c <= 0x7E && c != '|') out[used++] = (char)c;
    }
    out[used] = '\0';
    return used > 0;
}

esp_err_t ls_store_save_avoid_history(const char *text)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(text);
    if (len >= LS_AVOID_HISTORY_MAX) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    if (len == 0) {
        err = bsp_storage_erase(handle, KEY_AVOID_HISTORY);
    } else {
        err = bsp_storage_set_blob(handle, KEY_AVOID_HISTORY, text, len);
    }
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

esp_err_t ls_store_clear_avoid_history(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = bsp_storage_erase(handle, KEY_AVOID_HISTORY);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

bool ls_store_load_profile_id(char *out, size_t cap)
{
    if (!out || cap < LS_PROFILE_ID_MAX + 1) return false;
    out[0] = '\0';
    if (!s_ready) return false;

    nvs_handle_t handle;
    if (bsp_storage_open(LS_NVS_NAMESPACE, &handle) != ESP_OK) return false;

    // Stored without its terminator so the blob is exactly the id length; the
    // caller's buffer has room for the NUL.
    size_t len = 0;
    esp_err_t err = bsp_storage_get_blob(handle, KEY_PROFILE_ID, NULL, &len);
    if (err == ESP_OK && len == LS_PROFILE_ID_MAX) {
        err = bsp_storage_get_blob(handle, KEY_PROFILE_ID, out, &len);
    } else {
        err = ESP_ERR_INVALID_SIZE;
    }
    bsp_storage_close(handle);
    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }

    out[LS_PROFILE_ID_MAX] = '\0';
    if (!ls_profile_valid(out)) {
        // A partially written or hand-edited blob must not become a blob path.
        ESP_LOGW(TAG, "profile id 损坏,忽略");
        out[0] = '\0';
        return false;
    }
    return true;
}

esp_err_t ls_store_save_profile_id(const char *id)
{
    if (!ls_profile_valid(id)) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err == ESP_OK) {
        err = bsp_storage_set_blob(handle, KEY_PROFILE_ID, id,
                                   LS_PROFILE_ID_MAX);
        if (err == ESP_OK) err = bsp_storage_commit(handle);
        bsp_storage_close(handle);
    }
    return err;
}

esp_err_t ls_store_clear_profile_id(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err != ESP_OK) return err;
    err = bsp_storage_erase(handle, KEY_PROFILE_ID);
    if (err == ESP_OK) err = bsp_storage_erase(handle, KEY_BUDDY);
    if (err == ESP_OK) err = bsp_storage_commit(handle);
    bsp_storage_close(handle);
    return err;
}

bool ls_store_load_buddy(ls_buddy_t *out)
{
    if (!out) return false;
    if (!s_ready) return false;

    char buf[LS_BUDDY_TEXT_MAX];
    nvs_handle_t handle;
    if (bsp_storage_open(LS_NVS_NAMESPACE, &handle) != ESP_OK) return false;

    size_t len = sizeof(buf);
    esp_err_t err = bsp_storage_get_blob(handle, KEY_BUDDY, buf, &len);
    bsp_storage_close(handle);
    if (err != ESP_OK || len == 0 || len > sizeof(buf)) {
        return false;
    }

    if (ls_buddy_parse(buf, len, out) != LS_BUDDY_OK) {
        ESP_LOGW(TAG, "伙伴数据损坏,忽略(旧数据保留在 NVS 中以便排查)");
        return false;
    }
    return true;
}

esp_err_t ls_store_save_buddy(const ls_buddy_t *buddy)
{
    if (!buddy) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char buf[LS_BUDDY_TEXT_MAX];
    size_t len = 0;
    if (ls_buddy_write(buddy, buf, sizeof(buf), &len) != LS_BUDDY_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = bsp_storage_open(LS_NVS_NAMESPACE, &handle);
    if (err == ESP_OK) {
        err = bsp_storage_set_blob(handle, KEY_BUDDY, buf, len);
        if (err == ESP_OK) err = bsp_storage_commit(handle);
        bsp_storage_close(handle);
    }
    return err;
}
