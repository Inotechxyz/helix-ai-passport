// main/langstudy_model.h
// Shared data model for the offline English-study application.
//
// Everything here is plain C so the parsers, schedulers and synthesizer can be
// built and tested on the host without ESP-IDF.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Version 2 adds the UTF-8 Chinese translation field. Packs written by the
// version-1 format are rejected, so the application falls back to the built-in
// CET pack. Learning progress has its own format version (LS_PROGRESS_VERSION)
// and is not invalidated by this pack change.
#define LS_FORMAT_VERSION 2
// Number of words requested from DeepSeek for a refreshed pack. 60 keeps the
// streamed text buffer and the parsed model small enough to fit the SRAM-only
// ESP32-C3 alongside the Wi-Fi/TLS stacks. The built-in pack and packs already
// stored on devices may still hold up to LS_WORDS_MAX words.
#define LS_WORD_TARGET 60
// A pack the model stopped short is still usable when it contains at least
// this many complete WORD lines. The worker trims the partial tail and
// rewrites the header count instead of discarding the whole generation.
// 30 keeps two sessions (20 + 10 distinct words) available, so a deliberate
// partial reply is still worth accepting instead of repeating the request.
#define LS_WORD_MIN_ACCEPT 30

// Vocabulary tier requested from DeepSeek. Stored in NVS as a u8, so the
// numeric order must stay stable once devices are in the field.
typedef enum {
    LS_EXAM_CET4 = 0,
    LS_EXAM_CET6,
    LS_EXAM_IELTS,
    LS_EXAM_COUNT,
} ls_exam_t;

static inline const char *ls_exam_name(ls_exam_t exam)
{
    switch (exam) {
    case LS_EXAM_CET6: return "CET6";
    case LS_EXAM_IELTS: return "IELTS";
    case LS_EXAM_CET4:
    default: return "CET4";
    }
}
#define LS_SESSION_CARDS 20
#define LS_LESSONS_MAX 32
// Worst-case capacity, not the refresh target. Keeping 100 means packs written
// by older firmware and the 100-word built-in pack still parse after a refresh
// target change; the parser accepts any count from 1 to this limit.
#define LS_WORDS_MAX 100
#define LS_PRON_TOKENS_MAX 32
#define LS_PROGRESS_RECORDS_MAX 256

// Field limits are part of the on-flash format. Raising one invalidates older
// parsers, so change the format version together with them.
#define LS_LESSON_TITLE_MAX 24
#define LS_WORD_TEXT_MAX 24
#define LS_POS_MAX 5
#define LS_MEANING_MAX 40
#define LS_EXAMPLE_MAX 64
#define LS_PRON_MAX 48
// UTF-8 bytes, not characters: 48 bytes holds 16 CJK code points.
#define LS_TRANSLATION_MAX 48

// Upper bounds for the serialized text forms. The writer refuses to exceed them
// and the application sizes its download buffers from them. 24 KiB covers the
// worst legal 100-word pack (24-byte words, 48-byte Chinese translations,
// 64-byte examples and 48-byte pronunciations) plus the JSON wrapper that the
// streamed DeepSeek response carries, while still fitting the SRAM-only part.
#define LS_PACK_TEXT_MAX 24576

// Download buffer for a generated pack. 60 words at the prompt's field limits
// serialize to ~13.7 KiB (including the {"pack":"..."} wrapper around a
// newline-escaped inner pack), so 16 KiB leaves headroom while keeping the
// allocation small enough to survive alongside a live TLS session on the
// SRAM-only ESP32-C3. Parsing and storage still use the 24 KiB format bound so
// existing 100-word packs remain readable.
#define LS_GEN_PACK_TEXT_MAX 16384
#define LS_PROGRESS_TEXT_MAX 20480

// Space-separated snapshot of the current pack's words. It is sent with the
// refresh prompt so the model avoids reusing vocabulary the learner already
// has. Sized for 100 typical words (~9 bytes each) plus headroom; the builder
// stops cleanly on a word boundary if a pack of very long words overflows it.
#define LS_AVOID_WORDS_MAX 1600
// Rolling list of words from recently generated packs, oldest first. The active
// pack alone is replaced on every refresh, so without this history the third
// generation could reintroduce words from the first one. NVS-backed, trimmed
// from the front when new words no longer fit.
#define LS_AVOID_HISTORY_MAX 1600

// Success/failure codes shared by the pure modules.
typedef enum {
    LS_OK = 0,
    LS_ERR_ARG,
    LS_ERR_FORMAT,
    LS_ERR_CHECKSUM,
    LS_ERR_RANGE,
    LS_ERR_UNIQUE,
    LS_ERR_SIZE,
    LS_ERR_NO_MEM,
} ls_status_t;

typedef struct {
    uint32_t id;
    char title[LS_LESSON_TITLE_MAX + 1];
} ls_lesson_t;

typedef struct {
    uint32_t id;
    uint32_t lesson_id;
    char text[LS_WORD_TEXT_MAX + 1];
    char pos[LS_POS_MAX + 1];
    char meaning[LS_MEANING_MAX + 1];
    char translation[LS_TRANSLATION_MAX + 1];  // UTF-8 Chinese
    char example[LS_EXAMPLE_MAX + 1];
    char pron[LS_PRON_MAX + 1];
} ls_word_t;

typedef struct {
    uint16_t lesson_count;
    uint16_t word_count;
    ls_lesson_t lessons[LS_LESSONS_MAX];
    ls_word_t words[LS_WORDS_MAX];
} ls_pack_t;

// Staging model for an online-generated pack. The prompt asks for
// LS_WORD_TARGET words, so the worker can parse into this smaller structure
// (~15.5 KiB) instead of the 100-word ls_pack_t (25 KiB). On the SRAM-only
// ESP32-C3 the 25 KiB allocation fails after generation while the 16 KiB reply
// buffer is still live; the UI copies this into state->pack afterwards.
#define LS_GEN_WORDS_MAX LS_WORD_TARGET
typedef struct {
    uint16_t lesson_count;
    uint16_t word_count;
    ls_lesson_t lessons[LS_LESSONS_MAX];
    ls_word_t words[LS_GEN_WORDS_MAX];
} ls_gen_pack_t;

typedef struct {
    uint32_t word_id;
    uint32_t revision;
    uint32_t due_session;
    uint16_t correct;
    uint16_t wrong;
    uint8_t box; // 0..4
} ls_progress_record_t;

typedef struct {
    uint32_t session_count;
    uint32_t answered_total;
    uint32_t correct_total;
    uint16_t record_count;
    ls_progress_record_t records[LS_PROGRESS_RECORDS_MAX];
} ls_progress_t;
