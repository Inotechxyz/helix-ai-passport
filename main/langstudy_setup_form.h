// main/langstudy_setup_form.h
// Validation helpers for the phone-facing SoftAP setup page.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// application/x-www-form-urlencoded decoding. A decoded NUL byte is rejected
// because every consumer here works with C strings.
bool ls_form_url_decode(const char *in, size_t in_len,
                        char *out, size_t cap, size_t *out_len);

bool ls_form_get(const char *body, size_t len, const char *key,
                 char *out, size_t cap, size_t *out_len);

bool ls_setup_validate_ssid(const char *ssid);
bool ls_setup_validate_password(const char *password);
bool ls_setup_validate_api_key(const char *key);   // empty clears the key
bool ls_setup_validate_server_url(const char *url); // empty disables sync
