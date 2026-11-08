// main/langstudy_json.c
#include "langstudy_json.h"

#include <stdint.h>
#include <string.h>

bool ls_json_escape(const char *in, char *out, size_t cap, size_t *out_len)
{
    if (!in || !out || cap == 0 || !out_len) return false;
    size_t used = 0;

    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        const char *escape = NULL;
        char unicode[7];

        switch (c) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                unicode[0] = '\\';
                unicode[1] = 'u';
                unicode[2] = '0';
                unicode[3] = '0';
                unicode[4] = hex[(c >> 4) & 0x0F];
                unicode[5] = hex[c & 0x0F];
                unicode[6] = '\0';
                escape = unicode;
            }
            break;
        }

        if (escape) {
            size_t len = strlen(escape);
            if (used + len >= cap) return false;
            memcpy(out + used, escape, len);
            used += len;
        } else {
            if (used + 1 >= cap) return false;
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
    *out_len = used;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse one JSON string starting at *pos (which must point at the opening
// quote). When dst is non-NULL the decoded value is stored; otherwise the
// string is only validated/skipped. Returns false on malformed input.
static bool parse_string(const char *json, size_t len, size_t *pos,
                         char *dst, size_t cap, size_t *out_len)
{
    if (*pos >= len || json[*pos] != '"') return false;
    (*pos)++;

    size_t used = 0;
    while (*pos < len) {
        unsigned char c = (unsigned char)json[*pos];
        (*pos)++;

        if (c == '"') {
            if (dst) {
                if (used >= cap) return false;
                dst[used] = '\0';
            }
            if (out_len) *out_len = used;
            return true;
        }
        if (c < 0x20) return false;

        if (c == '\\') {
            if (*pos >= len) return false;
            char escape = json[(*pos)++];
            int decoded;
            switch (escape) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u': {
                if (*pos + 4 > len) return false;
                uint32_t value = 0;
                for (int i = 0; i < 4; i++) {
                    int digit = hex_value(json[*pos + i]);
                    if (digit < 0) return false;
                    value = (value << 4) | (uint32_t)digit;
                }
                *pos += 4;

                // The Chinese translation field arrives either as raw UTF-8
                // (handled by the default path above) or as \uXXXX escapes.
                // Decode a UTF-16 surrogate pair when the model emitted one.
                uint32_t codepoint = value;
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                    if (*pos + 6 > len || json[*pos] != '\\' ||
                        json[*pos + 1] != 'u') {
                        return false;
                    }
                    uint32_t low = 0;
                    for (int i = 0; i < 4; i++) {
                        int digit = hex_value(json[*pos + 2 + i]);
                        if (digit < 0) return false;
                        low = (low << 4) | (uint32_t)digit;
                    }
                    if (low < 0xDC00 || low > 0xDFFF) return false;
                    codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) +
                                (low - 0xDC00u);
                    *pos += 6;
                } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                    return false;
                }

                unsigned char encoded[4];
                size_t encoded_len;
                if (codepoint < 0x80) {
                    encoded[0] = (unsigned char)codepoint;
                    encoded_len = 1;
                } else if (codepoint < 0x800) {
                    encoded[0] = (unsigned char)(0xC0 | (codepoint >> 6));
                    encoded[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
                    encoded_len = 2;
                } else if (codepoint < 0x10000) {
                    encoded[0] = (unsigned char)(0xE0 | (codepoint >> 12));
                    encoded[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
                    encoded[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
                    encoded_len = 3;
                } else {
                    encoded[0] = (unsigned char)(0xF0 | (codepoint >> 18));
                    encoded[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
                    encoded[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
                    encoded[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
                    encoded_len = 4;
                }
                if (dst) {
                    if (used + encoded_len >= cap) return false;
                    memcpy(dst + used, encoded, encoded_len);
                    used += encoded_len;
                }
                continue;
            }
            default:
                return false;
            }
            if (dst) {
                if (used + 1 >= cap) return false;
                dst[used++] = (char)decoded;
            }
            continue;
        }

        if (dst) {
            if (used + 1 >= cap) return false;
            dst[used++] = (char)c;
        }
    }
    return false;
}

bool ls_json_extract_string(const char *json, size_t len, const char *key,
                            char *out, size_t cap, size_t *out_len)
{
    if (!json || !key || !out || cap == 0 || !out_len) return false;

    char key_buf[64];
    size_t pos = 0;
    while (pos < len) {
        unsigned char c = (unsigned char)json[pos];
        if (c != '"') {
            pos++;
            continue;
        }

        size_t key_len = 0;
        if (!parse_string(json, len, &pos, key_buf, sizeof(key_buf), &key_len)) {
            return false;
        }
        if (key_len != strlen(key) || memcmp(key_buf, key, key_len) != 0) {
            continue;
        }

        while (pos < len && (json[pos] == ' ' || json[pos] == '\t' ||
                             json[pos] == '\r' || json[pos] == '\n')) {
            pos++;
        }
        if (pos >= len || json[pos] != ':') continue;
        pos++;
        while (pos < len && (json[pos] == ' ' || json[pos] == '\t' ||
                             json[pos] == '\r' || json[pos] == '\n')) {
            pos++;
        }
        if (pos >= len || json[pos] != '"') return false;
        return parse_string(json, len, &pos, out, cap, out_len);
    }
    return false;
}
