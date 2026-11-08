// tests/test_langstudy_azure.c
// Host tests for Azure Blob URL validation, blob path building, and the
// x-ms-date spelling Azure expects.
#include "langstudy_azure.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

#define SAS_URL                                                              \
    "https://account.blob.core.windows.net/langstudy?sv=2022-11-02&ss=b"     \
    "&srt=co&sp=rcwl&se=2027-01-01T00%3A00%3A00Z"                            \
    "&st=2026-01-01T00%3A00%3A00Z&spr=https&sig=abc%2Bdef%2Fghi%3D"

static void test_url_validation(void)
{
    CHECK(!ls_azure_url_valid(NULL));
    CHECK(!ls_azure_url_valid(""));
    CHECK(ls_azure_url_valid(SAS_URL));

    // The SAS token is a bearer credential: plaintext must never be accepted.
    CHECK(!ls_azure_url_valid("http://account.blob.core.windows.net/c?sig=a"));
    CHECK(!ls_azure_url_valid("HTTPS://account.blob.core.windows.net/c?sig=a"));
    CHECK(!ls_azure_url_valid("ftp://account.blob.core.windows.net/c?sig=a"));

    // The query must carry a non-empty signature.
    CHECK(!ls_azure_url_valid(
        "https://account.blob.core.windows.net/langstudy"));
    CHECK(!ls_azure_url_valid(
        "https://account.blob.core.windows.net/langstudy?sv=2022-11-02"));
    CHECK(!ls_azure_url_valid(
        "https://account.blob.core.windows.net/langstudy?sig="));
    CHECK(!ls_azure_url_valid(
        "https://account.blob.core.windows.net/langstudy?sig=&sp=rw"));
    CHECK(ls_azure_url_valid(
        "https://account.blob.core.windows.net/langstudy?sig=a"));

    // A container segment is required.
    CHECK(!ls_azure_url_valid("https://account.blob.core.windows.net/?sig=a"));
    CHECK(!ls_azure_url_valid("https:///langstudy?sig=a"));
    CHECK(!ls_azure_url_valid("https://account.blob.core.windows.net?sig=a"));

    // Fragments, spaces and separators are not part of a container SAS.
    CHECK(!ls_azure_url_valid(
        "https://account.blob.core.windows.net/c?sig=a#fragment"));
    CHECK(!ls_azure_url_valid(
        "https://account.blob.core.windows.net/c?sig=a b"));
    CHECK(!ls_azure_url_valid(
        "https://account.blob.core.windows.net/c?sig=a|b"));

    // The configured limit is the container URL, not the blob URL.
    char too_long[LS_AZURE_URL_MAX + 16];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(!ls_azure_url_valid(too_long));

    char at_limit[LS_AZURE_URL_MAX + 1];
    const char *prefix = "https://acct.blob.core.windows.net/c?sig=";
    size_t prefix_len = strlen(prefix);
    memcpy(at_limit, prefix, prefix_len);
    memset(at_limit + prefix_len, 'a', LS_AZURE_URL_MAX - prefix_len);
    at_limit[LS_AZURE_URL_MAX] = '\0';
    CHECK(ls_azure_url_valid(at_limit));
}

static void test_blob_url(void)
{
    const char *id = "12345678-abcd-4f01-9ead-beef0badf00d";
    const char *query = strchr(SAS_URL, '?');
    char expected[LS_AZURE_BLOB_URL_MAX];
    (void)snprintf(expected, sizeof(expected),
                   "https://account.blob.core.windows.net/langstudy"
                   "/profiles/%s/progress.lsprog%s",
                   id, query);

    char url[LS_AZURE_BLOB_URL_MAX];
    CHECK(ls_azure_blob_url(SAS_URL, id, "progress.lsprog", url, sizeof(url)));
    CHECK(strcmp(url, expected) == 0);
    // The signature and its percent-encoding must survive untouched.
    CHECK(strstr(url, "sig=abc%2Bdef%2Fghi%3D") != NULL);

    // A trailing slash on the container URL must not double the separator.
    char slash_url[LS_AZURE_BLOB_URL_MAX];
    (void)snprintf(slash_url, sizeof(slash_url),
                   "https://account.blob.core.windows.net/langstudy/?sig=a");
    CHECK(ls_azure_blob_url(slash_url, id, "progress.lsprog", url,
                            sizeof(url)));
    CHECK(strcmp(url,
                 "https://account.blob.core.windows.net/langstudy"
                 "/profiles/12345678-abcd-4f01-9ead-beef0badf00d"
                 "/progress.lsprog?sig=a") == 0);

    CHECK(!ls_azure_blob_url(NULL, id, "progress.lsprog", url, sizeof(url)));
    CHECK(!ls_azure_blob_url(SAS_URL, NULL, "progress.lsprog", url,
                             sizeof(url)));
    CHECK(!ls_azure_blob_url(SAS_URL, id, NULL, url, sizeof(url)));

    // The profile id is a path segment: only the canonical UUID shape may be
    // spliced in, so "../" or uppercase spellings cannot escape the folder.
    CHECK(!ls_azure_blob_url(SAS_URL, "12345678-ABCD-4f01-9ead-beef0badf00d",
                             "progress.lsprog", url, sizeof(url)));
    CHECK(!ls_azure_blob_url(SAS_URL, "../12345678-abcd-4f01-9ead-beef0badf00d",
                             "progress.lsprog", url, sizeof(url)));
    CHECK(!ls_azure_blob_url(SAS_URL, "", "progress.lsprog", url, sizeof(url)));
    CHECK(!ls_azure_blob_url(SAS_URL, id, "../progress.lsprog", url,
                             sizeof(url)));
    CHECK(!ls_azure_blob_url(SAS_URL, id, "", url, sizeof(url)));

    char name_65[66];
    memset(name_65, 'a', 65);
    name_65[65] = '\0';
    CHECK(!ls_azure_blob_url(SAS_URL, id, name_65, url, sizeof(url)));

    // Refuse to truncate instead of uploading to a half-written path.
    char small[32];
    CHECK(!ls_azure_blob_url(SAS_URL, id, "progress.lsprog", small,
                             sizeof(small)));
    CHECK(!ls_azure_blob_url(SAS_URL, id, "progress.lsprog", url, 0));
}

static void test_http_date(void)
{
    char out[40];

    CHECK(ls_azure_http_date(0, out, sizeof(out)));
    CHECK(strcmp(out, "Thu, 01 Jan 1970 00:00:00 GMT") == 0);

    CHECK(ls_azure_http_date(951827696, out, sizeof(out)));
    CHECK(strcmp(out, "Tue, 29 Feb 2000 12:34:56 GMT") == 0);

    CHECK(ls_azure_http_date(1735689599, out, sizeof(out)));
    CHECK(strcmp(out, "Tue, 31 Dec 2024 23:59:59 GMT") == 0);

    CHECK(ls_azure_http_date(1794096000, out, sizeof(out)));
    CHECK(strcmp(out, "Sun, 08 Nov 2026 00:00:00 GMT") == 0);

    CHECK(!ls_azure_http_date(-1, out, sizeof(out)));
    CHECK(!ls_azure_http_date(0, out, 10));
    CHECK(!ls_azure_http_date(0, NULL, sizeof(out)));
}

int main(void)
{
    test_url_validation();
    test_blob_url();
    test_http_date();

    if (s_failures) {
        fprintf(stderr, "%d azure test(s) failed\n", s_failures);
        return 1;
    }
    printf("langstudy azure tests: PASS\n");
    return 0;
}
