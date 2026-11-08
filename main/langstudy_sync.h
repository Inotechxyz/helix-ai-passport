// main/langstudy_sync.h
// Progress sync through Azure Blob Storage.
//
// The device GETs profiles/<profile-id>/progress.lsprog from the container
// configured with a SAS URL, merges it with the local snapshot (local wins
// ties), then PUTs the merged document back. A missing blob is the first sync
// for that profile: the merged document is simply the local one.
#pragma once

#include "langstudy_model.h"
#include "langstudy_progress.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define LS_SYNC_DEFAULT_TIMEOUT_MS 20000

typedef struct {
    bool remote_found;  // the blob already existed
    bool created;       // this call uploaded the first copy
    int http_status;    // last Azure HTTP status, 0 for transport failures
} ls_sync_result_t;

// Requires an active Wi-Fi connection and a synced clock. container_sas is the
// HTTPS container SAS URL from Phone Setup; profile_id is a lowercase UUIDv4.
// On success `remote` holds the merged snapshot the caller should merge again
// and persist. The SAS is never logged.
esp_err_t ls_sync_exchange(const char *container_sas, const char *profile_id,
                           const ls_progress_t *local, ls_progress_t *remote,
                           ls_sync_result_t *result, uint32_t timeout_ms);
