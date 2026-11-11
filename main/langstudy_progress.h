// main/langstudy_progress.h
// Learning progress model and the LSPROG persistence format.
//
// LSPROG|1|<session_count>|<answered_total>|<correct_total>|<record_count>
// WORD|<word_id>|<box>|<correct>|<wrong>|<revision>|<due_session>
// END|<crc32>
#pragma once

#include "langstudy_model.h"

// The progress wire format is independent from the pack format: packs gained
// the translation field in v2 while LSPROG stayed at v1 so existing on-device
// records keep loading.
#define LS_PROGRESS_VERSION 1
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ls_progress_init(ls_progress_t *progress);

ls_status_t ls_progress_parse(const char *text, size_t len, ls_progress_t *out);
ls_status_t ls_progress_write(const ls_progress_t *progress,
                              char *out, size_t cap, size_t *out_len);
// Exact byte count ls_progress_write() would produce. Lets the sync path size
// its upload buffer instead of reserving LS_PROGRESS_TEXT_MAX while the TLS
// session is live.
ls_status_t ls_progress_write_size(const ls_progress_t *progress,
                                   size_t *out_len);

ls_progress_record_t *ls_progress_find(ls_progress_t *progress, uint32_t word_id);
const ls_progress_record_t *ls_progress_find_const(const ls_progress_t *progress,
                                                   uint32_t word_id);

// Return the record for word_id, creating a fresh box-0 record when needed.
// NULL means the progress table is full; call ls_progress_prune() first.
ls_progress_record_t *ls_progress_touch(ls_progress_t *progress, uint32_t word_id);

// Apply one answer. The card becomes due at
//   next_session_index + interval[box]
// where next_session_index = session_count + 2 (session_count is incremented
// only by ls_progress_finish_session()). interval is {0, 1, 3, 7, 14}.
void ls_progress_answer(ls_progress_t *progress, uint32_t word_id, bool correct);

void ls_progress_finish_session(ls_progress_t *progress);

// Merge an optional remote snapshot into the authoritative local one. Per-word
// revision wins; ties keep local. session_count and the counters take the
// snapshot from the side with more completed sessions.
bool ls_progress_merge(ls_progress_t *local, const ls_progress_t *remote);

// Drop records for words that are not in the active pack, oldest revision
// first, until the table has room again. Returns the number removed.
size_t ls_progress_prune(ls_progress_t *progress, const ls_pack_t *pack);
size_t ls_progress_prune_words(ls_progress_t *progress, const ls_word_t *words,
                               uint16_t count);

// Carry records across a pack refresh by matching word text (IDs may change
// between the built-in pack and generated packs). Returns the number copied.
// The _words variants let the UI compare the active pack with the smaller
// staging model returned by the online worker.
size_t ls_progress_remap_text(ls_progress_t *progress,
                              const ls_pack_t *old_pack,
                              const ls_pack_t *new_pack);
size_t ls_progress_remap_words(ls_progress_t *progress,
                               const ls_word_t *old_words, uint16_t old_count,
                               const ls_word_t *new_words, uint16_t new_count);

// Same remap as ls_progress_remap_words(), but the new words are the slice
// index of a generated pack text (see ls_pack_scan_view() in
// langstudy_content.h). The new IDs are computed exactly as
// ls_pack_assign_ids() will assign them once the UI parses the text. Call
// before the active pack is overwritten.
struct ls_pack_slice_view_t;
size_t ls_progress_remap_slices(ls_progress_t *progress,
                                const ls_pack_t *old_pack, const char *text,
                                const struct ls_pack_slice_view_t *scan);

// Remove every record whose word ID is absent from the pack. Call after
// ls_progress_remap_text() so the superseded IDs do not linger. Returns the
// number removed.
size_t ls_progress_drop_missing(ls_progress_t *progress, const ls_pack_t *pack);
size_t ls_progress_drop_missing_words(ls_progress_t *progress,
                                      const ls_word_t *words, uint16_t count);
