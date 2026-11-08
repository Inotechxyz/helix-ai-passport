// main/langstudy_gen.h
// DeepSeek request construction and response extraction.
#pragma once

#include "langstudy_model.h"
#include <stdbool.h>
#include <stddef.h>

// Space reserved for the fixed prompt body. The avoid list (up to
// LS_AVOID_WORDS_MAX bytes) is appended after it, so callers size their buffer
// as LS_GEN_PROMPT_BASE_MAX + LS_AVOID_WORDS_MAX.
#define LS_GEN_PROMPT_BASE_MAX 2944

// English prompt describing the LSPACK line format for the chosen vocabulary
// tier. The model is asked to omit END; the device appends a correct CRC for
// it.
// `avoid_words` (optional) is a space-separated list of words already in the
// learner's pack or its recent history; the model is told not to reuse them so
// each refresh yields fresh vocabulary. `strict_avoid` adds an explicit
// second-attempt warning after the device rejected a reply that repeated
// listed words.
bool ls_gen_build_prompt(char *out, size_t cap, size_t *out_len,
                         ls_exam_t exam, const char *avoid_words,
                         bool strict_avoid);

// Same prompt plus two extra selection hints. `seed` is mixed into the request
// as a fresh value on every generation, so the model cannot settle on the same
// "random" sample each time; `level` (1..10) is the requested vocabulary level,
// where 1 is starter and 10 is professional.
bool ls_gen_build_prompt_ex(char *out, size_t cap, size_t *out_len,
                            ls_exam_t exam, const char *avoid_words,
                            bool strict_avoid, uint32_t seed,
                            uint8_t level);

// Raise the user's starting level with cumulative learning progress, clamped to
// 1..10. Sessions count as a proxy for exposure, answers for effort, so a
// learner who keeps answering moves up even if many answers were wrong.
uint8_t ls_gen_level_for_progress(uint8_t base_level, uint32_t answered_total,
                                  uint32_t session_count);

// Append the words of a newly accepted pack to the rolling avoid history,
// keeping one space-separated list (oldest first) within `cap`. Duplicates are
// skipped case-insensitively and the oldest entries are removed from the front
// when the newest pack no longer fits, so a long-lived device keeps excluding
// reasonably recent vocabulary without unbounded growth.
void ls_gen_append_history(char *history, size_t cap, const ls_pack_t *pack);

// Build an OpenAI-compatible chat-completions request body. Escaping happens
// directly into `out`, so no prompt-sized scratch buffer is required.
bool ls_gen_build_request(char *out, size_t cap, const char *model,
                          const char *prompt, size_t *out_len);

// Byte sink used to emit the request without materializing it. The callback
// must consume all `len` bytes and return false on a short write.
typedef bool (*ls_gen_sink_t)(void *ctx, const char *data, size_t len);

// Exact Content-Length of the body emitted by ls_gen_emit_request(). Both use
// the same prefix strings and escaping rules, so they cannot drift.
size_t ls_gen_request_size(const char *model, const char *prompt);

// Emit the request body straight to a sink (for example esp_http_client_write)
// in small stack-buffer chunks. This removes the request-sized heap allocation
// that used to stay live through the TLS handshake on the SRAM-only device.
// Returns false if the sink rejects any byte.
bool ls_gen_emit_request(ls_gen_sink_t sink, void *ctx, const char *model,
                         const char *prompt);

// Pull the pack text out of an API response (choices[0].message.content), then
// out of the inner JSON object, normalize the trailing newline, and append the
// END|<crc32> line. The decode is in place, so `pack_out` is the only
// content-sized buffer needed.
bool ls_gen_extract_pack(const char *response, size_t len,
                         char *pack_out, size_t pack_cap, size_t *pack_len);

// Incremental variant of ls_gen_extract_pack for the HTTPS worker. Response
// bytes can be fed in arbitrarily small chunks, so the full JSON body never
// has to be resident: the unescaped value of the first "content" (or "pack")
// key is written straight into `out`. `out` must stay valid until finish.
// Mirrors the escape and key-matching rules of ls_json_extract_string().
//
// Allocate `out` only after the TLS handshake; the stream itself needs no
// other allocation. `cap` is typically LS_PACK_TEXT_MAX plus a little room for
// the {"pack":"..."} wrapper that DeepSeek may return inside content.
typedef struct {
    char *out;
    size_t cap;
    size_t used;
    size_t value_len;
    uint32_t codepoint;
    uint16_t pending_high;
    uint8_t state;
    uint8_t key_len;
    uint8_t key_state;   // 0 none, 1 matches "content", 2 matches "pack"
    uint8_t matched_key; // key whose value is being/been decoded
    uint8_t hex_count;
    bool failed;
} ls_gen_reader_t;

void ls_gen_reader_init(ls_gen_reader_t *reader, char *out, size_t cap);
// Feed the next response chunk. Returns false if the JSON is malformed or the
// value does not fit in `out`; the reader is then permanently failed.
bool ls_gen_reader_feed(ls_gen_reader_t *reader, const char *data, size_t len);
// Finish the stream and finalize the pack. Returns false when no value was
// found or the extraction/validation failed.
bool ls_gen_reader_finish(ls_gen_reader_t *reader, size_t *pack_len);

// Finalize content that was assembled by a streaming (SSE) reader: the bytes
// in `text` are the model's message content, either the LSPACK text itself or
// the {"pack":"..."} JSON object around it. Decodes in place, appends the END
// line and updates *len. `text` must have room for LS_PACK_TEXT_MAX plus the
// JSON wrapper; *len must end with a NUL slot available.
bool ls_gen_finalize_pack(char *text, size_t *len, size_t cap);

// Tolerate a generation that is short, has trailing prose, or carries fields
// the strict parser would reject. Finds the first LSPACK line, keeps only the
// complete LESSON/WORD lines, repairs each field (trim to the format limit,
// transliterate common Unicode punctuation, clear a phonetic that is not valid
// ARPAbet rather than dropping the word, drop words with unusable required
// fields), drops any partial tail or model-supplied END, rewrites the header
// with the actual counts, then appends a fresh END. Returns false when fewer
// than `min_words` usable WORD lines remain. Used for IELTS-sized packs, where
// the model sometimes stops a few lines early or answers with IPA phonetics
// even though most of the vocabulary is valid.
bool ls_gen_sanitize_pack(char *text, size_t *len, size_t cap,
                          uint16_t min_words);
