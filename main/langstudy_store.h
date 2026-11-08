// main/langstudy_store.h
// Power-loss-safe persistence for the active pack (A/B slots) and progress.
//
// Slot A/B protocol: write the new pack into the inactive slot, commit, then
// atomically flip the index. A power loss before the index commit leaves the
// previous pack authoritative; a corrupt slot is skipped on load.
#pragma once

#include "langstudy_model.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t slot;          // 0 or 1; 0xFF when the built-in pack is active
    uint32_t generation;   // increments on every successful pack save
    bool from_storage;     // false => built-in fallback
} ls_store_info_t;

// Wraps bsp_storage_init(). Failure is not fatal: the app runs with the
// built-in pack and in-RAM progress, but settings will not persist.
esp_err_t ls_store_init(void);
bool ls_store_ready(void);

// Load the newest valid pack. Falls back to the built-in pack when storage is
// unavailable or both slots fail to parse. Always fills *out on success.
bool ls_store_load_pack(ls_pack_t *out, ls_store_info_t *info);

// Validate and store pack text (header..END). Returns the new generation.
esp_err_t ls_store_save_pack(const char *text, size_t len,
                             uint32_t *out_generation);

// Serialize a validated pack model (with its current IDs) and store it.
esp_err_t ls_store_save_pack_model(const ls_pack_t *pack,
                                   uint32_t *out_generation);

// Remove both slots so the next boot falls back to the built-in pack.
esp_err_t ls_store_clear_pack(void);

// Missing/corrupt progress yields an initialized empty table and false.
bool ls_store_load_progress(ls_progress_t *out);
esp_err_t ls_store_save_progress(const ls_progress_t *progress);
esp_err_t ls_store_clear_progress(void);

// Rolling "recently generated words" list used to keep refreshes from cycling
// vocabulary. `out` receives a NUL-terminated space-separated list; a missing
// or malformed entry yields an empty string and false.
bool ls_store_load_avoid_history(char *out, size_t cap);
esp_err_t ls_store_save_avoid_history(const char *text);
esp_err_t ls_store_clear_avoid_history(void);
