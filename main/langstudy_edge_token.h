// main/langstudy_edge_token.h
// Edge Read Aloud (speech.platform.bing.com) v2 "Sec-MS-GEC" token.
//
// Microsoft's service accepts a token derived from the current time rounded
// down to five minutes. The algorithm is not documented; this is a faithful C
// port of edge-tts 7.3.1 (drm.py), which produces "<16 hex>-<64 hex>": a CS64
// mixing digest followed by a modified SHA-256 digest of
// "<windows-ticks><trusted-client-token>".
//
// The token is only valid with the matching client version reported in
// EDGE_GEC_VERSION / the User-Agent; when Microsoft rolls the client version
// these constants must be updated together.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// "<16 hex>-<64 hex>" = 81 characters.
#define LS_EDGE_GEC_TOKEN_LEN 81

// Build the token for a Unix timestamp in seconds. Returns false when the
// timestamp is unusable (before 2020), because such a clock cannot produce a
// token the service will accept.
bool ls_edge_gec_token(uint64_t unix_seconds,
                       char out[LS_EDGE_GEC_TOKEN_LEN + 1]);
