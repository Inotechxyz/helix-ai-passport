// main/langstudy_json.h
// Minimal JSON helpers for the DeepSeek request/response path.
//
// The device never needs a general DOM: it builds one request object and pulls
// two string values out of the response. Keeping the parser tiny avoids the
// heap fragmentation of cJSON on a no-PSRAM ESP32-C3.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Escape a NUL-terminated string for embedding in a JSON string literal.
bool ls_json_escape(const char *in, char *out, size_t cap, size_t *out_len);

// Find `key` and decode its string value into out (NUL-terminated). Matching is
// exact and case-sensitive. `\u` escapes (including surrogate pairs) are
// decoded to UTF-8 so Chinese translations survive either JSON spelling.
// When `out` aliases `json`, in-place decoding is supported because the decoded
// value is always written behind the read cursor.
bool ls_json_extract_string(const char *json, size_t len, const char *key,
                            char *out, size_t cap, size_t *out_len);
