// main/langstudy_content.h
// Parser, validator and serializer for the LSPACK content format.
//
// LSPACK|2|<lesson_count>|<word_count>
// LESSON|<id>|<title>
// WORD|<word_id>|<lesson_id>|<word>|<pos>|<meaning>|<translation>|<example>|<pron>
// END|<crc32>
//
// `translation` is UTF-8 Chinese. Every other field stays printable ASCII.
// The CRC covers every byte before the END line, including the trailing '\n'.
#pragma once

#include "langstudy_model.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t ls_crc32(const void *data, size_t len);

bool ls_pack_validate(const ls_pack_t *pack);

// A pack whose storage is supplied by the caller. Host tests and callers that
// only need a smaller word cap parse through this view; the online refresh path
// uses the allocation-free slice scan below instead.
typedef struct {
    uint16_t lesson_count;  // written by ls_pack_parse_view()
    uint16_t word_count;    // written by ls_pack_parse_view()
    uint16_t lesson_cap;
    uint16_t word_cap;
    ls_lesson_t *lessons;
    ls_word_t *words;
} ls_pack_view_t;

bool ls_pack_validate_view(const ls_pack_view_t *view);
ls_status_t ls_pack_parse_view(const char *text, size_t len,
                               ls_pack_view_t *out);

// A parsed pack that stores field offsets into the source text instead of
// copying strings. Used after an online generation because a content-sized
// model allocation can fail on the SRAM-only ESP32-C3 once Wi-Fi/TLS has
// fragmented the heap, while this index is a fixed ~2 KiB. The text must stay
// alive (and unchanged) for as long as the slices are used.
typedef struct {
    uint16_t off;  // byte offset from the start of the pack text
    uint16_t len;  // trimmed length
} ls_pack_slice_t;

typedef struct {
    uint32_t id;
    ls_pack_slice_t title;
} ls_lesson_slice_t;

typedef struct {
    uint32_t id;
    uint32_t lesson_id;
    ls_pack_slice_t text;
    ls_pack_slice_t pos;
    ls_pack_slice_t meaning;
    ls_pack_slice_t translation;
    ls_pack_slice_t example;
    ls_pack_slice_t pron;
} ls_word_slice_t;

typedef struct ls_pack_slice_view_t {
    uint16_t lesson_count;
    uint16_t word_count;
    ls_lesson_slice_t lessons[LS_LESSONS_MAX];
    ls_word_slice_t words[LS_GEN_WORDS_MAX];
} ls_pack_slice_view_t;

// Validate a pack and index its fields without allocating or copying strings.
// Applies the same checks as ls_pack_parse_view(), in the same order, and uses
// the generated-format caps (LS_GEN_WORDS_MAX); a successful result means
// ls_pack_parse() on the same text cannot fail.
ls_status_t ls_pack_scan_view(const char *text, size_t len,
                              ls_pack_slice_view_t *out);

// Copy one trimmed slice into a NUL-terminated buffer. Returns false when the
// slice does not fit (callers pass a cap above the format limit for the field).
bool ls_pack_slice_copy(const char *text, const ls_pack_slice_t *slice,
                        char *dst, size_t cap);

// CRC32 of the canonical body (the value carried by the END line). Returns 0
// for an invalid pack. Useful as a cheap pack identity for sync.
uint32_t ls_pack_crc(const ls_pack_t *pack);

ls_status_t ls_pack_parse(const char *text, size_t len, ls_pack_t *out);
ls_status_t ls_pack_write(const ls_pack_t *pack, char *out, size_t cap,
                          size_t *out_len);

// Exact serialized size including the terminating NUL. Returns 0 for an
// invalid pack or when the size exceeds LS_PACK_TEXT_MAX. Persistence uses
// this to malloc only what the pack needs instead of the whole format cap.
size_t ls_pack_text_size(const ls_pack_t *pack);

// Normalize a generated/built-in pack body: ensure a trailing newline, drop any
// existing END line, then append a freshly computed END|<crc32> line.
bool ls_pack_finalize(char *text, size_t *len, size_t cap);

const ls_word_t *ls_pack_find_word(const ls_pack_t *pack, uint32_t id);
const ls_word_t *ls_pack_find_text(const ls_pack_t *pack, const char *text);
bool ls_text_equals_ci(const char *a, const char *b);

// Stable generated-word identity: FNV-1a over the normalized, lower-cased text
// with bit 31 set so generated IDs never collide with built-in 1001..1100.
uint32_t ls_word_id_for_text(const char *text);
void ls_pack_assign_ids(ls_word_t *words, uint16_t count);
void ls_pack_assign_generated_ids(ls_pack_t *pack);

// Copy a validated staging pack into the full-size active model. The UI thread
// calls this after the worker has parsed the reply, so no second 25 KiB heap
// allocation is needed.
void ls_pack_copy_generated(ls_pack_t *dst, const ls_gen_pack_t *src);
