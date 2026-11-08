// main/langstudy_deepseek.h
// DeepSeek (OpenAI-compatible) pack generation over HTTPS.
//
// The device asks for a JSON object containing a "pack" string, drops any
// model-supplied END line, and appends its own CRC. The result is not parsed
// here; the caller must validate it against the active pack rules before use.
#pragma once

#include "langstudy_model.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Per-socket-operation timeout. Generation is consumed as SSE, so this only
// has to cover the gap between reads (headers and token deltas), not the whole
// completion; a 60-word pack can still take a minute or more to finish. DeepSeek
// queues requests when busy, so the first token may take well over a minute.
#define LS_GEN_DEFAULT_TIMEOUT_MS 120000

typedef struct {
    const char *api_key;     // required, "sk-..."
    const char *model;       // NULL => "deepseek-chat"
    uint32_t timeout_ms;     // 0 => LS_GEN_DEFAULT_TIMEOUT_MS
    ls_exam_t exam;          // vocabulary tier encoded in the prompt
    const char *avoid_words; // optional space-separated "do not reuse" list
    bool strict_avoid;       // retry after a repeated-word rejection
    uint32_t seed;           // fresh per request; varies the word sample
    uint8_t level;           // 1..10 English level requested from the model
} ls_deepseek_cfg_t;

// Generates a pack and extracts finalized LSPACK text (header..END) into one
// pack-sized heap buffer. The HTTPS body is unescaped as it arrives, so no
// full-response buffer is allocated. On success *pack_out is owned by the
// caller (free it) and *pack_len is its length; this keeps both the TLS
// handshake and the response peak low on the SRAM-only ESP32-C3.
esp_err_t ls_deepseek_generate(const ls_deepseek_cfg_t *cfg,
                               char **pack_out, size_t *pack_len);

// Cheap credential/connectivity check used by the status screen. Sends a GET to
// /models, which does not consume generation quota. `status_out` (optional)
// receives the HTTP status when a response was received. Returns ESP_OK only
// for HTTP 200.
esp_err_t ls_deepseek_probe(const ls_deepseek_cfg_t *cfg,
                            uint32_t timeout_ms, int *status_out);

// Human-readable detail for the most recent ls_deepseek_generate() failure.
// Never NULL; an empty string means the last call succeeded. The buffer lives
// in this module and is overwritten by the next generation attempt; copy it if
// it must outlive the call.
const char *ls_deepseek_last_error(void);
