// main/langstudy_setup_form.c
#include "langstudy_setup_form.h"

#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ls_form_url_decode(const char *in, size_t in_len,
                        char *out, size_t cap, size_t *out_len)
{
    if (!in || !out || cap == 0 || !out_len) return false;
    size_t used = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= in_len) return false;
            int high = hex_value(in[i + 1]);
            int low = hex_value(in[i + 2]);
            if (high < 0 || low < 0) return false;
            c = (char)((high << 4) | low);
            i += 2;
        }
        if (c == '\0') return false;
        if (used + 1 >= cap) return false;
        out[used++] = c;
    }
    out[used] = '\0';
    *out_len = used;
    return true;
}

static bool decode_key(const char *in, size_t in_len,
                       char *out, size_t cap)
{
    size_t len = 0;
    return ls_form_url_decode(in, in_len, out, cap, &len);
}

bool ls_form_get(const char *body, size_t len, const char *key,
                 char *out, size_t cap, size_t *out_len)
{
    if (!body || !key || !out || cap == 0 || !out_len) return false;
    *out_len = 0;

    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && body[end] != '&') end++;

        size_t equals = pos;
        while (equals < end && body[equals] != '=') equals++;

        char decoded_key[64];
        if (equals > pos &&
            decode_key(body + pos, equals - pos, decoded_key,
                       sizeof(decoded_key)) &&
            strcmp(decoded_key, key) == 0) {
            if (equals == end) {
                out[0] = '\0';
                *out_len = 0;
                return true;
            }
            return ls_form_url_decode(body + equals + 1, end - equals - 1,
                                      out, cap, out_len);
        }
        pos = end + 1;
    }
    return false;
}

bool ls_setup_validate_ssid(const char *ssid)
{
    if (!ssid) return false;
    size_t len = strlen(ssid);
    if (len == 0 || len > 32) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

bool ls_setup_validate_password(const char *password)
{
    if (!password) return false;
    size_t len = strlen(password);
    if (len == 0) return true; // open network
    if (len < 8 || len > 63) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)password[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

bool ls_setup_validate_api_key(const char *key)
{
    if (!key) return false;
    size_t len = strlen(key);
    if (len == 0) return true; // clear the stored key
    if (len < 8 || len > 200) return false;
    if (strncmp(key, "sk-", 3) != 0) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c < 0x21 || c > 0x7E) return false;
    }
    return true;
}

bool ls_setup_validate_server_url(const char *url)
{
    if (!url) return false;
    size_t len = strlen(url);
    if (len == 0) return true; // sync disabled
    if (len > 160) return false;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c > 0x7E || c == '|') return false;
    }
    return true;
}
