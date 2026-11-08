// main/langstudy_sync.h
// Optional progress sync. The device POSTs its LSPROG document to a
// user-configured URL; the server merges per-word revisions and returns the
// merged LSPROG document. The caller re-merges (idempotent) and persists.
#pragma once

#include "langstudy_model.h"
#include "langstudy_progress.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define LS_SYNC_DEFAULT_TIMEOUT_MS 20000

// Requires an active Wi-Fi connection. The URL may be http:// (LAN) or
// https:// (validated against the certificate bundle).
esp_err_t ls_sync_exchange(const char *url, uint32_t pack_crc,
                           const ls_progress_t *local,
                           ls_progress_t *remote,
                           uint32_t timeout_ms);
