// main/langstudy_online.h
// Background worker for the two optional online actions. It owns Wi-Fi,
// SNTP, the HTTP request, and the heap result objects; the UI thread receives
// them through ls_online_poll() and is the only code that mutates the active
// pack/progress.
#pragma once

#include "langstudy_model.h"
#include "langstudy_progress.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LS_ONLINE_REFRESH = 0,   // generate a new 60-word pack via DeepSeek
    LS_ONLINE_SYNC,          // exchange progress with Azure Blob Storage
    LS_ONLINE_CHECK,         // report Wi-Fi and DeepSeek reachability
    LS_ONLINE_PROBE,         // silently re-check both links after boot/wake
} ls_online_op_t;

typedef enum {
    LS_ONLINE_REFRESH_OK = 0,
    LS_ONLINE_REFRESH_FAIL,
    LS_ONLINE_SYNC_OK,
    LS_ONLINE_SYNC_FAIL,
    LS_ONLINE_CHECK_OK,
    LS_ONLINE_CHECK_FAIL,
    LS_ONLINE_PROBE_DONE,    // link flags only; the UI shows no screen
} ls_online_msg_kind_t;

#define LS_ONLINE_MESSAGE_MAX 160
// Longer diagnostic text shown behind "DOWN: DETAIL"; empty when the failure
// needs no extra explanation.
#define LS_ONLINE_DETAIL_MAX 224

// Which link results the message carries. The UI only updates its status icons
// for hops that this operation actually exercised, so an offline refresh does
// not clear a green Wi-Fi icon learned from an earlier check.
#define LS_LINK_WIFI_KNOWN 0x01u
#define LS_LINK_WIFI_OK 0x02u
#define LS_LINK_DEEPSEEK_KNOWN 0x04u
#define LS_LINK_DEEPSEEK_OK 0x08u

typedef struct {
    ls_online_msg_kind_t kind;
    char message[LS_ONLINE_MESSAGE_MAX];
    char detail[LS_ONLINE_DETAIL_MAX]; // optional diagnostic explanation
    // REFRESH_OK: finalized pack text, caller owns. Handed over as text (not a
    // parsed model) because a content-sized allocation after Wi-Fi/TLS is not
    // reliable on the SRAM-only ESP32-C3; the UI parses it into its resident
    // pack. May carry a trailing NUL but text_len is authoritative.
    char *text;
    size_t text_len;
    ls_progress_t *progress;  // SYNC_OK: remote snapshot, caller owns
    uint8_t link_flags;       // LS_LINK_* bit mask
} ls_online_msg_t;

esp_err_t ls_online_init(void);
void ls_online_shutdown(void);

// Snapshot config/progress on the calling (UI) thread and queue the work.
// Returns false when busy or the worker is not running.
bool ls_online_start(ls_online_op_t op);

// Non-blocking. On true the caller owns msg->text / msg->progress.
bool ls_online_poll(ls_online_msg_t *msg);

bool ls_online_busy(void);

// True while the busy worker is only running LS_ONLINE_PROBE. The voice worker
// uses this to wait for the boot check instead of falling back to the offline
// voice; any other operation keeps the existing fallback.
bool ls_online_probe_active(void);
