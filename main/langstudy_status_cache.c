// main/langstudy_status_cache.c
// Deep sleep is a reset: every static in normal RAM comes back zero. The three
// header values are the last known state, so they live in RTC memory, which
// keeps its bytes across esp_deep_sleep_start() and is re-initialized by a
// power-on reset - exactly the lifetime the header indicators promise.
#include "langstudy_status_cache.h"

#include "esp_attr.h"

#include <stdint.h>

#define LS_STATUS_CACHE_MAGIC 0x4C535343UL   // "LSSC"
#define LS_STATUS_SOC_UNKNOWN (-1)

typedef struct {
    uint32_t magic;
    uint8_t wifi_ok;
    uint8_t deepseek_ok;
    int16_t soc;
} rtc_status_cache_t;

static RTC_DATA_ATTR rtc_status_cache_t s_cache;

bool ls_status_cache_get(ls_status_cache_t *out)
{
    if (!out) return false;
    if (s_cache.magic != LS_STATUS_CACHE_MAGIC) {
        out->wifi_ok = false;
        out->deepseek_ok = false;
        out->soc = LS_STATUS_SOC_UNKNOWN;
        return false;
    }
    out->wifi_ok = s_cache.wifi_ok != 0;
    out->deepseek_ok = s_cache.deepseek_ok != 0;
    out->soc = s_cache.soc;
    return true;
}

void ls_status_cache_set_links(bool wifi_ok, bool deepseek_ok)
{
    if (s_cache.magic != LS_STATUS_CACHE_MAGIC) {
        s_cache.magic = LS_STATUS_CACHE_MAGIC;
        s_cache.soc = LS_STATUS_SOC_UNKNOWN;
    }
    s_cache.wifi_ok = wifi_ok ? 1 : 0;
    s_cache.deepseek_ok = deepseek_ok ? 1 : 0;
}

void ls_status_cache_set_soc(int soc)
{
    if (soc < 0 || soc > 100) return;
    if (s_cache.magic != LS_STATUS_CACHE_MAGIC) {
        s_cache.magic = LS_STATUS_CACHE_MAGIC;
        s_cache.wifi_ok = 0;
        s_cache.deepseek_ok = 0;
    }
    s_cache.soc = (int16_t)soc;
}
