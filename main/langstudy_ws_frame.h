// main/langstudy_ws_frame.h
// Minimal RFC 6455 frame encoder/header parser. The Edge TTS client is the only
// user: it sends masked text frames and reads unmasked server frames. Keeping
// these helpers free of ESP-IDF dependencies lets the host tests exercise the
// length encoding and fragmentation rules directly.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_WS_OPCODE_CONTINUATION 0x0
#define LS_WS_OPCODE_TEXT 0x1
#define LS_WS_OPCODE_BINARY 0x2
#define LS_WS_OPCODE_CLOSE 0x8
#define LS_WS_OPCODE_PING 0x9
#define LS_WS_OPCODE_PONG 0xA

typedef struct {
    bool fin;
    uint8_t opcode;
    bool masked;
    uint8_t mask[4];
    uint64_t payload_len;
    size_t header_len;  // bytes to skip before the payload
} ls_ws_header_t;

// Encode one client frame. Client frames are always masked and un-fragmented.
// Returns the total byte count written to out, or 0 when cap is too small.
size_t ls_ws_build_frame(uint8_t opcode, const void *payload, size_t len,
                         const uint8_t mask[4], uint8_t *out, size_t cap);

// Parse a frame header. Returns false until at least header_len bytes are
// available or when the header is malformed. Payload bytes are not consumed.
bool ls_ws_parse_header(const uint8_t *data, size_t avail,
                        ls_ws_header_t *out);
