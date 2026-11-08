// main/langstudy_session.h
// Deterministic session scheduling: due reviews first, then new words.
#pragma once

#include "langstudy_model.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A session normally holds up to LS_SESSION_CARDS words. When fewer unseen or
// due words remain, the builder fills from the rest of the pack so a session
// still reaches this minimum (unless the pack itself is smaller).
#define LS_SESSION_MIN 10

// Fill out_ids with at most LS_SESSION_CARDS word IDs and return the count.
// Due records are ordered by (due_session, word_id). New words are taken from a
// rotating window so repeated sessions do not always start with the same word.
size_t ls_session_build(const ls_pack_t *pack, const ls_progress_t *progress,
                        uint32_t out_ids[LS_SESSION_CARDS]);

// Variant used by the UI: `exclude` holds the previous session's word IDs.
// Those words are skipped while enough other words exist, so consecutive
// sessions contain a different set; they are only reused to reach
// LS_SESSION_MIN. `exclude` may be NULL or empty.
size_t ls_session_build_ex(const ls_pack_t *pack, const ls_progress_t *progress,
                           const uint32_t *exclude, size_t exclude_len,
                           uint32_t out_ids[LS_SESSION_CARDS]);

// A record is due at the start of session (session_count + 1).
bool ls_session_record_due(const ls_progress_record_t *record,
                           uint32_t session_count);
