// main/langstudy_azure.h
// Pure helpers for Azure Blob Storage progress sync.
//
// The device is given a container SAS URL (Blob service) from Phone Setup. The
// profile id becomes a virtual folder, so each profile owns
//   profiles/<profile-id>/progress.lsprog
// inside that container. Keeping URL building and date formatting free of
// ESP-IDF lets the host tests pin the spelling Azure expects.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Longest container SAS URL accepted from Phone Setup. Azure portal links are
// typically well under 400 characters; 512 leaves room for edited expiry.
#define LS_AZURE_URL_MAX 512

// A blob URL adds "/profiles/<36-char id>/progress.lsprog" to the container
// URL, so it needs a little more room than the configured SAS.
#define LS_AZURE_BLOB_URL_MAX (LS_AZURE_URL_MAX + 96)

// True for an HTTPS container SAS URL: printable ASCII, a container path
// before the query, and a non-empty "sig" parameter. An empty string is not
// valid here; callers treat empty as "sync disabled".
bool ls_azure_url_valid(const char *container_sas);

// Build "<container>/profiles/<profile_id>/<blob_name><query>". The query
// string (including '?') is copied verbatim after the blob name. profile_id
// must be a lowercase UUIDv4 and blob_name a plain file name.
bool ls_azure_blob_url(const char *container_sas, const char *profile_id,
                       const char *blob_name, char *out, size_t cap);

// Format an RFC 1123 GMT date, e.g. "Sun, 08 Nov 2026 12:34:56 GMT", for the
// x-ms-date header. Implemented without time-zone or locale state so host and
// firmware produce identical bytes.
bool ls_azure_http_date(int64_t unix_time, char *out, size_t cap);
