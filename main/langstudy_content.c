// main/langstudy_content.c
#include "langstudy_content.h"

#include "langstudy_phoneme.h"

#include <stdio.h>
#include <string.h>

#define LINE_MAX_BYTES 320
#define FIELD_MAX 10

static uint32_t update_crc(uint32_t crc, const void *data, size_t len);

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

static size_t bounded_strlen(const char *text, size_t limit)
{
    size_t len = 0;
    while (len < limit && text[len] != '\0') len++;
    return len;
}

static bool field_chars_ok(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7E || c == '|') return false;
    }
    return true;
}

// UTF-8 fields (currently only `translation`) may carry CJK code points, but
// must still be valid UTF-8 so the display layer never receives truncated
// sequences. Control characters and the field separator stay forbidden.
static bool utf8_chars_ok(const char *text, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 && c <= 0x7E) {
            if (c == '|') return false;
            i++;
            continue;
        }
        if (c < 0x80) return false;

        size_t extra;
        uint32_t codepoint;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            codepoint = c & 0x1Fu;
            if (codepoint < 2) return false;  // overlong
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            codepoint = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            codepoint = c & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= len) return false;  // truncated sequence
        for (size_t k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)text[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (cc & 0x3Fu);
        }
        if ((extra == 2 && codepoint < 0x800) ||
            (extra == 3 && (codepoint < 0x10000 || codepoint > 0x10FFFF))) {
            return false;
        }
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;
        if (codepoint == '|' || codepoint < 0x20) return false;
        i += extra + 1;
    }
    return true;
}

static bool has_non_ascii(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)text[i] >= 0x80) return true;
    }
    return false;
}

static void trim_bounds(const char *src, size_t len,
                        size_t *begin, size_t *end)
{
    size_t b = 0;
    size_t e = len;
    while (b < e && src[b] == ' ') b++;
    while (e > b && src[e - 1] == ' ') e--;
    *begin = b;
    *end = e;
}

// Copy a trimmed, printable-ASCII field. Returns false when it exceeds max_len
// or contains a character the line format cannot carry.
static bool copy_field(char *dst, size_t cap, const char *src, size_t len,
                       size_t max_len)
{
    size_t begin;
    size_t end;
    trim_bounds(src, len, &begin, &end);
    size_t trimmed = end - begin;
    if (trimmed > max_len || trimmed + 1 > cap) return false;
    if (!field_chars_ok(src + begin, trimmed)) return false;
    memcpy(dst, src + begin, trimmed);
    dst[trimmed] = '\0';
    return true;
}

// Copy a trimmed UTF-8 field (Chinese translation). ASCII-only input is
// accepted here so tests and legacy packs still parse, but the model validator
// requires at least one non-ASCII code point.
static bool copy_field_utf8(char *dst, size_t cap, const char *src, size_t len,
                            size_t max_len)
{
    size_t begin;
    size_t end;
    trim_bounds(src, len, &begin, &end);
    size_t trimmed = end - begin;
    if (trimmed > max_len || trimmed + 1 > cap) return false;
    if (!utf8_chars_ok(src + begin, trimmed)) return false;
    memcpy(dst, src + begin, trimmed);
    dst[trimmed] = '\0';
    return true;
}

bool ls_text_equals_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

uint32_t ls_word_id_for_text(const char *text)
{
    if (!text) return 0x80000000u;

    uint32_t hash = 2166136261u;
    bool pending_space = false;
    bool any = false;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ') {
            if (any) pending_space = true;
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (pending_space) {
            hash ^= (uint32_t)' ';
            hash *= 16777619u;
            pending_space = false;
        }
        hash ^= (uint32_t)c;
        hash *= 16777619u;
        any = true;
    }
    return hash | 0x80000000u;
}

// ---------------------------------------------------------------------------
// CRC32 (reflected, polynomial 0xEDB88320)
// ---------------------------------------------------------------------------

uint32_t ls_crc32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
// Field splitting
// ---------------------------------------------------------------------------

typedef struct {
    const char *ptr;
    size_t len;
} field_t;

static size_t split_line(const char *line, size_t len, char sep,
                         field_t *fields, size_t max_fields)
{
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || line[i] == sep) {
            if (count >= max_fields) return max_fields + 1;
            fields[count].ptr = line + start;
            fields[count].len = i - start;
            count++;
            start = i + 1;
        }
    }
    return count;
}

static bool parse_u32(const field_t *field, uint32_t *out)
{
    if (!field || field->len == 0 || field->len > 10) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < field->len; i++) {
        char c = field->ptr[i];
        if (c < '0' || c > '9') return false;
        value = value * 10u + (uint64_t)(c - '0');
        if (value > 0xFFFFFFFFull) return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool hex_digit(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

static bool parse_hex32(const field_t *field, uint32_t *out)
{
    if (!field || field->len != 8) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        uint8_t digit;
        if (!hex_digit(field->ptr[i], &digit)) return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

static const ls_lesson_t *find_lesson_in(const ls_lesson_t *lessons,
                                         uint16_t count, uint32_t id)
{
    for (uint16_t i = 0; i < count; i++) {
        if (lessons[i].id == id) return &lessons[i];
    }
    return NULL;
}

static bool validate_pack_data(const ls_lesson_t *lessons,
                               uint16_t lesson_count, const ls_word_t *words,
                               uint16_t word_count)
{
    if (!lessons || !words) return false;
    if (lesson_count == 0 || word_count == 0) return false;

    for (uint16_t i = 0; i < lesson_count; i++) {
        const ls_lesson_t *lesson = &lessons[i];
        if (lesson->id == 0) return false;
        size_t title_len = bounded_strlen(lesson->title, LS_LESSON_TITLE_MAX + 1);
        if (title_len == 0 || title_len > LS_LESSON_TITLE_MAX) return false;
        if (!field_chars_ok(lesson->title, title_len)) return false;
        if (lesson->title[0] == ' ' || lesson->title[title_len - 1] == ' ') return false;
        for (uint16_t j = 0; j < i; j++) {
            if (lessons[j].id == lesson->id) return false;
        }
    }

    for (uint16_t i = 0; i < word_count; i++) {
        const ls_word_t *word = &words[i];
        if (word->id == 0) return false;
        if (!find_lesson_in(lessons, lesson_count, word->lesson_id)) return false;

        const struct {
            const char *text;
            size_t len;
            size_t max;
            bool required;
            bool utf8;
        } fields[] = {
            {word->text, bounded_strlen(word->text, LS_WORD_TEXT_MAX + 1),
             LS_WORD_TEXT_MAX, true, false},
            {word->pos, bounded_strlen(word->pos, LS_POS_MAX + 1), LS_POS_MAX, false,
             false},
            {word->meaning, bounded_strlen(word->meaning, LS_MEANING_MAX + 1),
             LS_MEANING_MAX, true, false},
            {word->translation,
             bounded_strlen(word->translation, LS_TRANSLATION_MAX + 1),
             LS_TRANSLATION_MAX, true, true},
            {word->example, bounded_strlen(word->example, LS_EXAMPLE_MAX + 1),
             LS_EXAMPLE_MAX, false, false},
            {word->pron, bounded_strlen(word->pron, LS_PRON_MAX + 1), LS_PRON_MAX,
             false, false},
        };
        for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
            if (fields[f].len > fields[f].max) return false;
            if (fields[f].required && fields[f].len == 0) return false;
            bool chars_ok = fields[f].utf8
                                ? utf8_chars_ok(fields[f].text, fields[f].len)
                                : field_chars_ok(fields[f].text, fields[f].len);
            if (!chars_ok) return false;
            if (fields[f].len && (fields[f].text[0] == ' ' ||
                                  fields[f].text[fields[f].len - 1] == ' ')) {
                return false;
            }
        }
        // The product requirement is a Chinese translation, so reject a pack
        // whose model answered with English only.
        if (!has_non_ascii(word->translation,
                           bounded_strlen(word->translation,
                                          LS_TRANSLATION_MAX + 1))) {
            return false;
        }
        if (!ls_pron_validate(word->pron)) return false;

        for (uint16_t j = 0; j < i; j++) {
            if (words[j].id == word->id) return false;
            if (ls_text_equals_ci(words[j].text, word->text)) return false;
        }
    }
    return true;
}

bool ls_pack_validate(const ls_pack_t *pack)
{
    if (!pack) return false;
    // The refresh target is 60, but the built-in pack and packs written by
    // older firmware still hold 100 words. Accept any count the model fits.
    if (pack->lesson_count > LS_LESSONS_MAX ||
        pack->word_count > LS_WORDS_MAX) {
        return false;
    }
    return validate_pack_data(pack->lessons, pack->lesson_count, pack->words,
                              pack->word_count);
}

bool ls_pack_validate_view(const ls_pack_view_t *view)
{
    if (!view || !view->lessons || !view->words) return false;
    if (view->lesson_count > view->lesson_cap ||
        view->word_count > view->word_cap) {
        return false;
    }
    return validate_pack_data(view->lessons, view->lesson_count, view->words,
                              view->word_count);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

ls_status_t ls_pack_parse_view(const char *text, size_t len,
                               ls_pack_view_t *out)
{
    if (!text || !out || !out->lessons || !out->words) return LS_ERR_ARG;
    if (out->lesson_cap == 0 || out->word_cap == 0) return LS_ERR_ARG;
    if (len == 0 || len > LS_PACK_TEXT_MAX) return LS_ERR_SIZE;
    if (len < 2 || text[len - 1] != '\n') return LS_ERR_FORMAT;
    if (memchr(text, '\0', len)) return LS_ERR_FORMAT;

    memset(out->lessons, 0, sizeof(*out->lessons) * out->lesson_cap);
    memset(out->words, 0, sizeof(*out->words) * out->word_cap);
    out->lesson_count = 0;
    out->word_count = 0;

    size_t pos = 0;
    bool header_seen = false;
    bool end_seen = false;
    uint32_t expected_lessons = 0;
    uint32_t expected_words = 0;
    uint32_t crc = 0xFFFFFFFFu;

    while (pos < len) {
        const char *line = text + pos;
        const char *newline = memchr(line, '\n', len - pos);
        if (!newline) return LS_ERR_FORMAT;
        size_t line_len = (size_t)(newline - line);
        if (line_len > LINE_MAX_BYTES) return LS_ERR_SIZE;
        if (memchr(line, '\r', line_len)) return LS_ERR_FORMAT;

        field_t fields[FIELD_MAX];
        size_t count = split_line(line, line_len, '|', fields,
                                  sizeof(fields) / sizeof(fields[0]));
        if (count == 0 || count > FIELD_MAX) return LS_ERR_FORMAT;

        if (!header_seen) {
            uint32_t version;
            if (count != 4 || fields[0].len != 6 ||
                memcmp(fields[0].ptr, "LSPACK", 6) != 0 ||
                !parse_u32(&fields[1], &version) || version != LS_FORMAT_VERSION ||
                !parse_u32(&fields[2], &expected_lessons) ||
                !parse_u32(&fields[3], &expected_words)) {
                return LS_ERR_FORMAT;
            }
            if (expected_lessons == 0 || expected_lessons > out->lesson_cap ||
                expected_words == 0 || expected_words > out->word_cap) {
                return LS_ERR_RANGE;
            }
            header_seen = true;
            crc = update_crc(crc, line, line_len + 1);
            pos += line_len + 1;
            continue;
        }

        if (count >= 1 && fields[0].len == 3 &&
            memcmp(fields[0].ptr, "END", 3) == 0) {
            uint32_t expected_crc;
            if (count != 2 || !parse_hex32(&fields[1], &expected_crc)) {
                return LS_ERR_FORMAT;
            }
            if (~crc != expected_crc) return LS_ERR_CHECKSUM;
            if (pos + line_len + 1 != len) return LS_ERR_FORMAT;
            end_seen = true;
            pos = len;
            break;
        }

        if (count >= 1 && fields[0].len == 6 &&
            memcmp(fields[0].ptr, "LESSON", 6) == 0) {
            if (count != 3) return LS_ERR_FORMAT;
            if (out->lesson_count >= out->lesson_cap) return LS_ERR_RANGE;
            ls_lesson_t *lesson = &out->lessons[out->lesson_count];
            if (!parse_u32(&fields[1], &lesson->id) || lesson->id == 0) {
                return LS_ERR_RANGE;
            }
            if (!copy_field(lesson->title, sizeof(lesson->title),
                            fields[2].ptr, fields[2].len,
                            LS_LESSON_TITLE_MAX)) {
                return LS_ERR_RANGE;
            }
            if (lesson->title[0] == '\0') return LS_ERR_FORMAT;
            out->lesson_count++;
        } else if (count >= 1 && fields[0].len == 4 &&
                   memcmp(fields[0].ptr, "WORD", 4) == 0) {
            if (count != 9) return LS_ERR_FORMAT;
            if (out->word_count >= out->word_cap) return LS_ERR_RANGE;
            ls_word_t *word = &out->words[out->word_count];
            if (!parse_u32(&fields[1], &word->id) || word->id == 0) {
                return LS_ERR_RANGE;
            }
            if (!parse_u32(&fields[2], &word->lesson_id) || word->lesson_id == 0) {
                return LS_ERR_RANGE;
            }
            if (!copy_field(word->text, sizeof(word->text),
                            fields[3].ptr, fields[3].len, LS_WORD_TEXT_MAX) ||
                !copy_field(word->pos, sizeof(word->pos),
                            fields[4].ptr, fields[4].len, LS_POS_MAX) ||
                !copy_field(word->meaning, sizeof(word->meaning),
                            fields[5].ptr, fields[5].len, LS_MEANING_MAX) ||
                !copy_field_utf8(word->translation, sizeof(word->translation),
                                 fields[6].ptr, fields[6].len,
                                 LS_TRANSLATION_MAX) ||
                !copy_field(word->example, sizeof(word->example),
                            fields[7].ptr, fields[7].len, LS_EXAMPLE_MAX) ||
                !copy_field(word->pron, sizeof(word->pron),
                            fields[8].ptr, fields[8].len, LS_PRON_MAX)) {
                return LS_ERR_RANGE;
            }
            if (word->text[0] == '\0' || word->meaning[0] == '\0' ||
                word->translation[0] == '\0') {
                return LS_ERR_FORMAT;
            }
            out->word_count++;
        } else {
            return LS_ERR_FORMAT;
        }

        crc = update_crc(crc, line, line_len + 1);
        pos += line_len + 1;
    }

    if (!header_seen || !end_seen) return LS_ERR_FORMAT;
    if (out->lesson_count != expected_lessons ||
        out->word_count != expected_words) {
        return LS_ERR_RANGE;
    }
    return ls_pack_validate_view(out) ? LS_OK : LS_ERR_FORMAT;
}

ls_status_t ls_pack_parse(const char *text, size_t len, ls_pack_t *out)
{
    if (!out) return LS_ERR_ARG;
    ls_pack_view_t view = {
        .lesson_cap = LS_LESSONS_MAX,
        .word_cap = LS_WORDS_MAX,
        .lessons = out->lessons,
        .words = out->words,
    };
    ls_status_t status = ls_pack_parse_view(text, len, &view);
    out->lesson_count = status == LS_OK ? view.lesson_count : 0;
    out->word_count = status == LS_OK ? view.word_count : 0;
    return status;
}

// ---------------------------------------------------------------------------
// Slice scan
// ---------------------------------------------------------------------------
//
// The online worker must validate a generated pack without allocating a
// content-sized model: after Wi-Fi/TLS the ESP32-C3 heap reports tens of
// kilobytes free but the largest contiguous block is only a few kilobytes, so
// malloc(sizeof(ls_gen_pack_t)) fails. This scan applies the same checks as
// ls_pack_parse_view() and records where each field lives in the reply text.

bool ls_pack_slice_copy(const char *text, const ls_pack_slice_t *slice,
                        char *dst, size_t cap)
{
    if (!text || !slice || !dst || cap == 0) return false;
    if ((size_t)slice->len + 1 > cap) return false;
    memcpy(dst, text + slice->off, slice->len);
    dst[slice->len] = '\0';
    return true;
}

static bool slice_equals_ci(const char *text, const ls_pack_slice_t *a,
                            const ls_pack_slice_t *b)
{
    if (a->len != b->len) return false;
    for (uint16_t i = 0; i < a->len; i++) {
        unsigned char ca = (unsigned char)text[a->off + i];
        unsigned char cb = (unsigned char)text[b->off + i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

// Trim one field and store its bounds. Returns false when the trimmed value
// exceeds the format limit or carries a character the line format forbids;
// callers treat that like copy_field's failure (LS_ERR_RANGE).
static bool scan_slice(const char *text, const field_t *field, size_t max_len,
                       bool utf8, ls_pack_slice_t *out)
{
    size_t begin;
    size_t end;
    trim_bounds(field->ptr, field->len, &begin, &end);
    size_t trimmed = end - begin;
    out->off = (uint16_t)(field->ptr + begin - text);
    out->len = (uint16_t)trimmed;
    if (trimmed > max_len) return false;
    return utf8 ? utf8_chars_ok(field->ptr + begin, trimmed)
                : field_chars_ok(field->ptr + begin, trimmed);
}

static const ls_lesson_slice_t *scan_find_lesson(const ls_lesson_slice_t *lessons,
                                                 uint16_t count, uint32_t id)
{
    for (uint16_t i = 0; i < count; i++) {
        if (lessons[i].id == id) return &lessons[i];
    }
    return NULL;
}

ls_status_t ls_pack_scan_view(const char *text, size_t len,
                              ls_pack_slice_view_t *out)
{
    if (!text || !out) return LS_ERR_ARG;
    if (len == 0 || len > LS_PACK_TEXT_MAX) return LS_ERR_SIZE;
    if (len < 2 || text[len - 1] != '\n') return LS_ERR_FORMAT;
    if (memchr(text, '\0', len)) return LS_ERR_FORMAT;

    out->lesson_count = 0;
    out->word_count = 0;

    size_t pos = 0;
    bool header_seen = false;
    bool end_seen = false;
    uint32_t expected_lessons = 0;
    uint32_t expected_words = 0;
    uint32_t crc = 0xFFFFFFFFu;

    while (pos < len) {
        const char *line = text + pos;
        const char *newline = memchr(line, '\n', len - pos);
        if (!newline) return LS_ERR_FORMAT;
        size_t line_len = (size_t)(newline - line);
        if (line_len > LINE_MAX_BYTES) return LS_ERR_SIZE;
        if (memchr(line, '\r', line_len)) return LS_ERR_FORMAT;

        field_t fields[FIELD_MAX];
        size_t count = split_line(line, line_len, '|', fields,
                                  sizeof(fields) / sizeof(fields[0]));
        if (count == 0 || count > FIELD_MAX) return LS_ERR_FORMAT;

        if (!header_seen) {
            uint32_t version;
            if (count != 4 || fields[0].len != 6 ||
                memcmp(fields[0].ptr, "LSPACK", 6) != 0 ||
                !parse_u32(&fields[1], &version) || version != LS_FORMAT_VERSION ||
                !parse_u32(&fields[2], &expected_lessons) ||
                !parse_u32(&fields[3], &expected_words)) {
                return LS_ERR_FORMAT;
            }
            if (expected_lessons == 0 || expected_lessons > LS_LESSONS_MAX ||
                expected_words == 0 || expected_words > LS_GEN_WORDS_MAX) {
                return LS_ERR_RANGE;
            }
            header_seen = true;
            crc = update_crc(crc, line, line_len + 1);
            pos += line_len + 1;
            continue;
        }

        if (count >= 1 && fields[0].len == 3 &&
            memcmp(fields[0].ptr, "END", 3) == 0) {
            uint32_t expected_crc;
            if (count != 2 || !parse_hex32(&fields[1], &expected_crc)) {
                return LS_ERR_FORMAT;
            }
            if (~crc != expected_crc) return LS_ERR_CHECKSUM;
            if (pos + line_len + 1 != len) return LS_ERR_FORMAT;
            end_seen = true;
            pos = len;
            break;
        }

        if (count >= 1 && fields[0].len == 6 &&
            memcmp(fields[0].ptr, "LESSON", 6) == 0) {
            if (count != 3) return LS_ERR_FORMAT;
            if (out->lesson_count >= LS_LESSONS_MAX) return LS_ERR_RANGE;
            ls_lesson_slice_t *lesson = &out->lessons[out->lesson_count];
            if (!parse_u32(&fields[1], &lesson->id) || lesson->id == 0) {
                return LS_ERR_RANGE;
            }
            if (!scan_slice(text, &fields[2], LS_LESSON_TITLE_MAX, false,
                            &lesson->title)) {
                return LS_ERR_RANGE;
            }
            if (lesson->title.len == 0) return LS_ERR_FORMAT;
            out->lesson_count++;
        } else if (count >= 1 && fields[0].len == 4 &&
                   memcmp(fields[0].ptr, "WORD", 4) == 0) {
            if (count != 9) return LS_ERR_FORMAT;
            if (out->word_count >= LS_GEN_WORDS_MAX) return LS_ERR_RANGE;
            ls_word_slice_t *word = &out->words[out->word_count];
            if (!parse_u32(&fields[1], &word->id) || word->id == 0) {
                return LS_ERR_RANGE;
            }
            if (!parse_u32(&fields[2], &word->lesson_id) || word->lesson_id == 0) {
                return LS_ERR_RANGE;
            }
            if (!scan_slice(text, &fields[3], LS_WORD_TEXT_MAX, false,
                            &word->text) ||
                !scan_slice(text, &fields[4], LS_POS_MAX, false, &word->pos) ||
                !scan_slice(text, &fields[5], LS_MEANING_MAX, false,
                            &word->meaning) ||
                !scan_slice(text, &fields[6], LS_TRANSLATION_MAX, true,
                            &word->translation) ||
                !scan_slice(text, &fields[7], LS_EXAMPLE_MAX, false,
                            &word->example) ||
                !scan_slice(text, &fields[8], LS_PRON_MAX, false,
                            &word->pron)) {
                return LS_ERR_RANGE;
            }
            if (word->text.len == 0 || word->meaning.len == 0 ||
                word->translation.len == 0) {
                return LS_ERR_FORMAT;
            }
            out->word_count++;
        } else {
            return LS_ERR_FORMAT;
        }

        crc = update_crc(crc, line, line_len + 1);
        pos += line_len + 1;
    }

    if (!header_seen || !end_seen) return LS_ERR_FORMAT;
    if (out->lesson_count != expected_lessons ||
        out->word_count != expected_words) {
        return LS_ERR_RANGE;
    }
    if (out->lesson_count == 0 || out->word_count == 0) return LS_ERR_FORMAT;

    // Cross-field checks ls_pack_parse_view() performs after the CRC walk
    // (validate_pack_data), in the same order, so both return the same status
    // for every input.
    for (uint16_t i = 0; i < out->lesson_count; i++) {
        for (uint16_t j = 0; j < i; j++) {
            if (out->lessons[j].id == out->lessons[i].id) return LS_ERR_FORMAT;
        }
    }
    for (uint16_t i = 0; i < out->word_count; i++) {
        const ls_word_slice_t *word = &out->words[i];
        if (!scan_find_lesson(out->lessons, out->lesson_count,
                              word->lesson_id)) {
            return LS_ERR_FORMAT;
        }
        if (!has_non_ascii(text + word->translation.off,
                           word->translation.len)) {
            return LS_ERR_FORMAT;
        }
        char pron[LS_PRON_MAX + 1];
        if (!ls_pack_slice_copy(text, &word->pron, pron, sizeof(pron)) ||
            !ls_pron_validate(pron)) {
            return LS_ERR_FORMAT;
        }
        for (uint16_t j = 0; j < i; j++) {
            if (out->words[j].id == word->id) return LS_ERR_FORMAT;
            if (slice_equals_ci(text, &out->words[j].text, &word->text)) {
                return LS_ERR_FORMAT;
            }
        }
    }
    return LS_OK;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

typedef struct {
    char *out;
    size_t cap;
    size_t used;
    uint32_t crc;
    bool overflow;
} line_writer_t;

static uint32_t update_crc(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return crc;
}

static void writer_line(line_writer_t *w, const char *line)
{
    if (w->overflow) return;
    size_t len = strlen(line);
    if (w->used + len > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->out + w->used, line, len);
    w->used += len;
    w->crc = update_crc(w->crc, line, len);
}

uint32_t ls_pack_crc(const ls_pack_t *pack)
{
    if (!pack || !ls_pack_validate(pack)) return 0;

    char line[LINE_MAX_BYTES];
    uint32_t crc = 0xFFFFFFFFu;
    int written = snprintf(line, sizeof(line), "LSPACK|%d|%u|%u\n",
                           LS_FORMAT_VERSION, (unsigned)pack->lesson_count,
                           (unsigned)pack->word_count);
    if (written < 0 || (size_t)written >= sizeof(line)) return 0;
    crc = update_crc(crc, line, (size_t)written);

    for (uint16_t i = 0; i < pack->lesson_count; i++) {
        const ls_lesson_t *lesson = &pack->lessons[i];
        written = snprintf(line, sizeof(line), "LESSON|%lu|%s\n",
                           (unsigned long)lesson->id, lesson->title);
        if (written < 0 || (size_t)written >= sizeof(line)) return 0;
        crc = update_crc(crc, line, (size_t)written);
    }
    for (uint16_t i = 0; i < pack->word_count; i++) {
        const ls_word_t *word = &pack->words[i];
        written = snprintf(line, sizeof(line),
                           "WORD|%lu|%lu|%s|%s|%s|%s|%s|%s\n",
                           (unsigned long)word->id,
                           (unsigned long)word->lesson_id,
                           word->text, word->pos, word->meaning,
                           word->translation, word->example, word->pron);
        if (written < 0 || (size_t)written >= sizeof(line)) return 0;
        crc = update_crc(crc, line, (size_t)written);
    }
    return ~crc;
}

ls_status_t ls_pack_write(const ls_pack_t *pack, char *out, size_t cap,
                          size_t *out_len)
{
    if (!pack || !out || !out_len) return LS_ERR_ARG;
    *out_len = 0;
    if (!ls_pack_validate(pack)) return LS_ERR_FORMAT;

    line_writer_t writer = {
        .out = out,
        .cap = cap,
        .used = 0,
        .crc = 0xFFFFFFFFu,
        .overflow = false,
    };
    char line[LINE_MAX_BYTES];

    int written = snprintf(line, sizeof(line), "LSPACK|%d|%u|%u\n",
                           LS_FORMAT_VERSION, (unsigned)pack->lesson_count,
                           (unsigned)pack->word_count);
    if (written < 0 || (size_t)written >= sizeof(line)) return LS_ERR_SIZE;
    writer_line(&writer, line);

    for (uint16_t i = 0; i < pack->lesson_count; i++) {
        const ls_lesson_t *lesson = &pack->lessons[i];
        written = snprintf(line, sizeof(line), "LESSON|%lu|%s\n",
                           (unsigned long)lesson->id, lesson->title);
        if (written < 0 || (size_t)written >= sizeof(line)) return LS_ERR_SIZE;
        writer_line(&writer, line);
    }
    for (uint16_t i = 0; i < pack->word_count; i++) {
        const ls_word_t *word = &pack->words[i];
        written = snprintf(line, sizeof(line),
                           "WORD|%lu|%lu|%s|%s|%s|%s|%s|%s\n",
                           (unsigned long)word->id,
                           (unsigned long)word->lesson_id,
                           word->text, word->pos, word->meaning,
                           word->translation, word->example, word->pron);
        if (written < 0 || (size_t)written >= sizeof(line)) return LS_ERR_SIZE;
        writer_line(&writer, line);
    }

    if (writer.overflow) return LS_ERR_SIZE;
    uint32_t final_crc = ~writer.crc;
    written = snprintf(line, sizeof(line), "END|%08lX\n",
                       (unsigned long)final_crc);
    if (written < 0 || (size_t)written >= sizeof(line)) return LS_ERR_SIZE;
    size_t end_len = (size_t)written;
    if (writer.used + end_len > cap) return LS_ERR_SIZE;
    memcpy(out + writer.used, line, end_len);
    writer.used += end_len;
    *out_len = writer.used;
    return LS_OK;
}

size_t ls_pack_text_size(const ls_pack_t *pack)
{
    if (!pack || !ls_pack_validate(pack)) return 0;

    char line[LINE_MAX_BYTES];
    size_t total = 0;
    int written = snprintf(line, sizeof(line), "LSPACK|%d|%u|%u\n",
                           LS_FORMAT_VERSION, (unsigned)pack->lesson_count,
                           (unsigned)pack->word_count);
    if (written < 0 || (size_t)written >= sizeof(line)) return 0;
    total += (size_t)written;

    for (uint16_t i = 0; i < pack->lesson_count; i++) {
        const ls_lesson_t *lesson = &pack->lessons[i];
        written = snprintf(line, sizeof(line), "LESSON|%lu|%s\n",
                           (unsigned long)lesson->id, lesson->title);
        if (written < 0 || (size_t)written >= sizeof(line)) return 0;
        total += (size_t)written;
    }
    for (uint16_t i = 0; i < pack->word_count; i++) {
        const ls_word_t *word = &pack->words[i];
        written = snprintf(line, sizeof(line),
                           "WORD|%lu|%lu|%s|%s|%s|%s|%s|%s\n",
                           (unsigned long)word->id,
                           (unsigned long)word->lesson_id,
                           word->text, word->pos, word->meaning,
                           word->translation, word->example, word->pron);
        if (written < 0 || (size_t)written >= sizeof(line)) return 0;
        total += (size_t)written;
    }

    written = snprintf(line, sizeof(line), "END|%08lX\n", 0UL);
    if (written < 0 || (size_t)written >= sizeof(line)) return 0;
    total += (size_t)written;
    if (total >= LS_PACK_TEXT_MAX) return 0;
    return total + 1;  // terminating NUL
}

bool ls_pack_finalize(char *text, size_t *len, size_t cap)
{
    if (!text || !len || cap == 0) return false;
    size_t used = *len;
    if (used == 0 || used > cap) return false;

    if (text[used - 1] != '\n') {
        if (used + 1 >= cap) return false;
        text[used++] = '\n';
    }

    // Drop a caller/model-supplied END line so the CRC is always recomputed.
    // Skip the terminating newline first: otherwise the scan starts past the
    // last line and never sees its "END|" prefix.
    size_t line_start = used;
    if (line_start > 0 && text[line_start - 1] == '\n') line_start--;
    while (line_start > 0 && text[line_start - 1] != '\n') line_start--;
    if (line_start < used && used - line_start >= 4 &&
        memcmp(text + line_start, "END|", 4) == 0) {
        used = line_start;
    }
    if (used == 0) return false;

    uint32_t crc = ls_crc32(text, used);
    char end_line[16];
    int written = snprintf(end_line, sizeof(end_line), "END|%08lX\n",
                           (unsigned long)crc);
    if (written < 0 || (size_t)written >= sizeof(end_line)) return false;
    size_t end_len = (size_t)written;
    if (used + end_len >= cap) return false;
    memcpy(text + used, end_line, end_len);
    used += end_len;
    text[used] = '\0';
    *len = used;
    return true;
}

// ---------------------------------------------------------------------------
// Lookup / normalization
// ---------------------------------------------------------------------------

const ls_word_t *ls_pack_find_word(const ls_pack_t *pack, uint32_t id)
{
    if (!pack) return NULL;
    for (uint16_t i = 0; i < pack->word_count; i++) {
        if (pack->words[i].id == id) return &pack->words[i];
    }
    return NULL;
}

const ls_word_t *ls_pack_find_text(const ls_pack_t *pack, const char *text)
{
    if (!pack || !text) return NULL;
    for (uint16_t i = 0; i < pack->word_count; i++) {
        if (ls_text_equals_ci(pack->words[i].text, text)) return &pack->words[i];
    }
    return NULL;
}

void ls_pack_assign_ids(ls_word_t *words, uint16_t count)
{
    if (!words) return;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t id = ls_word_id_for_text(words[i].text);
        for (uint16_t j = 0; j < i; j++) {
            if (words[j].id == id) {
                id++;
                j = (uint16_t)-1; // restart the collision scan
            }
        }
        words[i].id = id;
    }
}

void ls_pack_assign_generated_ids(ls_pack_t *pack)
{
    if (!pack) return;
    ls_pack_assign_ids(pack->words, pack->word_count);
}

void ls_pack_copy_generated(ls_pack_t *dst, const ls_gen_pack_t *src)
{
    if (!dst || !src) return;
    memset(dst, 0, sizeof(*dst));

    uint16_t lessons = src->lesson_count;
    if (lessons > LS_LESSONS_MAX) lessons = LS_LESSONS_MAX;
    uint16_t words = src->word_count;
    if (words > LS_WORDS_MAX) words = LS_WORDS_MAX;

    dst->lesson_count = lessons;
    dst->word_count = words;
    memcpy(dst->lessons, src->lessons, sizeof(ls_lesson_t) * lessons);
    memcpy(dst->words, src->words, sizeof(ls_word_t) * words);
}
