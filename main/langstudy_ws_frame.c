// main/langstudy_ws_frame.c
#include "langstudy_ws_frame.h"

#include <string.h>

static size_t header_size(uint64_t len, bool masked)
{
    size_t size = 2;
    if (len >= 65536) {
        size += 8;
    } else if (len >= 126) {
        size += 2;
    }
    if (masked) size += 4;
    return size;
}

size_t ls_ws_build_frame(uint8_t opcode, const void *payload, size_t len,
                         const uint8_t mask[4], uint8_t *out, size_t cap)
{
    if (!out || !mask || (len > 0 && !payload)) return 0;
    if (len > 0xFFFFFFFFu) return 0;

    size_t total = header_size(len, true) + len;
    if (total > cap) return 0;

    uint8_t *p = out;
    *p++ = (uint8_t)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        *p++ = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        *p++ = 0x80 | 126;
        *p++ = (uint8_t)(len >> 8);
        *p++ = (uint8_t)len;
    } else {
        *p++ = 0x80 | 127;
        uint64_t wide = (uint64_t)len;
        for (int i = 7; i >= 0; i--) *p++ = (uint8_t)(wide >> (i * 8));
    }
    memcpy(p, mask, 4);
    p += 4;
    const uint8_t *src = (const uint8_t *)payload;
    for (size_t i = 0; i < len; i++) p[i] = src[i] ^ mask[i % 4];
    return total;
}

bool ls_ws_parse_header(const uint8_t *data, size_t avail,
                        ls_ws_header_t *out)
{
    if (!data || !out || avail < 2) return false;

    ls_ws_header_t header;
    memset(&header, 0, sizeof(header));
    header.fin = (data[0] & 0x80) != 0;
    header.opcode = (uint8_t)(data[0] & 0x0F);
    header.masked = (data[1] & 0x80) != 0;
    uint64_t len = data[1] & 0x7F;

    size_t pos = 2;
    if (len == 126) {
        if (avail < pos + 2) return false;
        len = ((uint64_t)data[pos] << 8) | data[pos + 1];
        pos += 2;
    } else if (len == 127) {
        if (avail < pos + 8) return false;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | data[pos + i];
        pos += 8;
    }

    if (header.masked) {
        if (avail < pos + 4) return false;
        memcpy(header.mask, data + pos, 4);
        pos += 4;
    }

    header.payload_len = len;
    header.header_len = pos;
    *out = header;
    return true;
}
