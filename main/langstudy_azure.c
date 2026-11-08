// main/langstudy_azure.c
#include "langstudy_azure.h"

#include "langstudy_buddy.h"

#include <stdio.h>
#include <string.h>

#define HTTPS_PREFIX "https://"
#define HTTPS_PREFIX_LEN 8

static bool blob_name_valid(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0 || len > 64) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// The query starts after '?'. sig must be a parameter on its own, with a value.
static bool has_sig_param(const char *query)
{
    const char *p = query;
    while (*p) {
        if (strncmp(p, "sig=", 4) == 0) {
            return p[4] != '\0' && p[4] != '&';
        }
        const char *amp = strchr(p, '&');
        if (!amp) return false;
        p = amp + 1;
    }
    return false;
}

bool ls_azure_url_valid(const char *container_sas)
{
    if (!container_sas) return false;
    size_t len = strlen(container_sas);
    if (len == 0 || len > LS_AZURE_URL_MAX) return false;
    if (strncmp(container_sas, HTTPS_PREFIX, HTTPS_PREFIX_LEN) != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)container_sas[i];
        if (c <= 0x20 || c > 0x7E || c == '|' || c == '#') return false;
    }

    const char *host = container_sas + HTTPS_PREFIX_LEN;
    const char *query = strchr(host, '?');
    if (!query || query == host || query[1] == '\0') return false;

    // Require a host and a container segment before the query:
    // https://host/container?...
    const char *slash = strchr(host, '/');
    if (!slash || slash == host || slash >= query || slash + 1 == query) {
        return false;
    }

    return has_sig_param(query + 1);
}

bool ls_azure_blob_url(const char *container_sas, const char *profile_id,
                       const char *blob_name, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!ls_azure_url_valid(container_sas) || !ls_profile_valid(profile_id)) {
        return false;
    }
    if (!blob_name_valid(blob_name)) return false;

    const char *query = strchr(container_sas, '?');
    if (!query) return false;
    size_t base_len = (size_t)(query - container_sas);
    // Tolerate a trailing slash on the container URL so the path never doubles.
    while (base_len > 0 && container_sas[base_len - 1] == '/') base_len--;

    int written = snprintf(out, cap, "%.*s/profiles/%s/%s%s", (int)base_len,
                           container_sas, profile_id, blob_name, query);
    return written > 0 && (size_t)written < cap;
}

bool ls_azure_http_date(int64_t unix_time, char *out, size_t cap)
{
    if (!out || cap == 0 || unix_time < 0) return false;

    int64_t days = unix_time / 86400;
    int64_t seconds = unix_time % 86400;

    // 1970-01-01 was a Thursday; Sunday is 0.
    int weekday = (int)((days + 4) % 7);
    if (weekday < 0) weekday += 7;

    // Howard Hinnant's days-from-civil inverse. Shifting the epoch to March
    // makes leap days the last day of the year, which removes special cases.
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp < 10 ? mp + 3 : mp - 9;
    year += (month <= 2);

    if (year < 1 || year > 9999) return false;

    static const char *const weekdays[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    };
    static const char *const months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    int written = snprintf(out, cap, "%s, %02u %s %04d %02u:%02u:%02u GMT",
                           weekdays[weekday], day, months[month - 1],
                           (int)year, (unsigned)(seconds / 3600),
                           (unsigned)((seconds % 3600) / 60),
                           (unsigned)(seconds % 60));
    return written > 0 && (size_t)written < cap;
}
