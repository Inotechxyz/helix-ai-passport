// main/langstudy_status_cache.h
// Last-known header status across a deep-sleep wake.
#pragma once

#include <stdbool.h>

typedef struct {
    bool wifi_ok;
    bool deepseek_ok;
    int soc;   // -1 = never read
} ls_status_cache_t;

// False before the first store (power-on/reflash); *out is then zeroed with
// soc = -1.
bool ls_status_cache_get(ls_status_cache_t *out);

void ls_status_cache_set_links(bool wifi_ok, bool deepseek_ok);
void ls_status_cache_set_soc(int soc);
