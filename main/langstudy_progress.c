// main/langstudy_progress.c
#include "langstudy_progress.h"

#include "langstudy_content.h"

#include <stdio.h>
#include <string.h>

#define LINE_MAX_BYTES 160
#define FIELD_MAX 8

static const uint8_t s_intervals[5] = {0, 1, 3, 7, 14};

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

static bool parse_u32(const char *text, size_t len, uint32_t *out)
{
    if (!text || len == 0 || len > 10) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10u + (uint64_t)(text[i] - '0');
        if (value > 0xFFFFFFFFull) return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_hex32(const char *text, size_t len, uint32_t *out)
{
    if (len != 8) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        char c = text[i];
        uint8_t digit;
        if (c >= '0' && c <= '9') digit = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint8_t)(c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

typedef struct {
    const char *ptr;
    size_t len;
} field_t;

static size_t split_line(const char *line, size_t len,
                         field_t *fields, size_t max_fields)
{
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || line[i] == '|') {
            if (count >= max_fields) return max_fields + 1;
            fields[count].ptr = line + start;
            fields[count].len = i - start;
            count++;
            start = i + 1;
        }
    }
    return count;
}

void ls_progress_init(ls_progress_t *progress)
{
    if (progress) memset(progress, 0, sizeof(*progress));
}

static bool progress_valid(const ls_progress_t *progress)
{
    if (!progress) return false;
    if (progress->record_count > LS_PROGRESS_RECORDS_MAX) return false;
    if (progress->correct_total > progress->answered_total) return false;
    for (uint16_t i = 0; i < progress->record_count; i++) {
        const ls_progress_record_t *record = &progress->records[i];
        if (record->word_id == 0 || record->box > 4) return false;
        for (uint16_t j = 0; j < i; j++) {
            if (progress->records[j].word_id == record->word_id) return false;
        }
    }
    return true;
}

ls_progress_record_t *ls_progress_find(ls_progress_t *progress, uint32_t word_id)
{
    if (!progress) return NULL;
    for (uint16_t i = 0; i < progress->record_count; i++) {
        if (progress->records[i].word_id == word_id) return &progress->records[i];
    }
    return NULL;
}

const ls_progress_record_t *ls_progress_find_const(const ls_progress_t *progress,
                                                   uint32_t word_id)
{
    if (!progress) return NULL;
    for (uint16_t i = 0; i < progress->record_count; i++) {
        if (progress->records[i].word_id == word_id) return &progress->records[i];
    }
    return NULL;
}

ls_progress_record_t *ls_progress_touch(ls_progress_t *progress, uint32_t word_id)
{
    if (!progress || word_id == 0) return NULL;
    ls_progress_record_t *record = ls_progress_find(progress, word_id);
    if (record) return record;
    if (progress->record_count >= LS_PROGRESS_RECORDS_MAX) return NULL;

    record = &progress->records[progress->record_count++];
    memset(record, 0, sizeof(*record));
    record->word_id = word_id;
    return record;
}

static void increment_u32(uint32_t *value)
{
    if (*value != UINT32_MAX) (*value)++;
}

static void increment_u16(uint16_t *value)
{
    if (*value != UINT16_MAX) (*value)++;
}

void ls_progress_answer(ls_progress_t *progress, uint32_t word_id, bool correct)
{
    if (!progress) return;
    ls_progress_record_t *record = ls_progress_touch(progress, word_id);
    if (!record) return;

    if (correct) {
        if (record->box < 4) record->box++;
        increment_u16(&record->correct);
    } else {
        record->box = 0;
        increment_u16(&record->wrong);
    }
    // session_count is only incremented when the session completes, so the
    // next session's 1-based index is session_count + 2.
    record->due_session = progress->session_count + 2 + s_intervals[record->box];
    increment_u32(&record->revision);

    increment_u32(&progress->answered_total);
    if (correct) increment_u32(&progress->correct_total);
}

void ls_progress_finish_session(ls_progress_t *progress)
{
    if (progress) increment_u32(&progress->session_count);
}

ls_status_t ls_progress_parse(const char *text, size_t len, ls_progress_t *out)
{
    if (!text || !out) return LS_ERR_ARG;
    if (len == 0 || len > LS_PROGRESS_TEXT_MAX) return LS_ERR_SIZE;
    if (len < 2 || text[len - 1] != '\n') return LS_ERR_FORMAT;
    if (memchr(text, '\0', len)) return LS_ERR_FORMAT;

    ls_progress_init(out);

    size_t pos = 0;
    bool header_seen = false;
    bool end_seen = false;
    uint32_t expected_records = 0;
    uint32_t crc = 0xFFFFFFFFu;

    while (pos < len) {
        const char *line = text + pos;
        const char *newline = memchr(line, '\n', len - pos);
        if (!newline) return LS_ERR_FORMAT;
        size_t line_len = (size_t)(newline - line);
        if (line_len > LINE_MAX_BYTES) return LS_ERR_SIZE;
        if (memchr(line, '\r', line_len)) return LS_ERR_FORMAT;

        field_t fields[FIELD_MAX];
        size_t count = split_line(line, line_len, fields,
                                  sizeof(fields) / sizeof(fields[0]));
        if (count == 0 || count > FIELD_MAX) return LS_ERR_FORMAT;

        if (!header_seen) {
            uint32_t version;
            if (count != 6 || fields[0].len != 6 ||
                memcmp(fields[0].ptr, "LSPROG", 6) != 0 ||
                !parse_u32(fields[1].ptr, fields[1].len, &version) ||
                version != LS_PROGRESS_VERSION ||
                !parse_u32(fields[2].ptr, fields[2].len, &out->session_count) ||
                !parse_u32(fields[3].ptr, fields[3].len, &out->answered_total) ||
                !parse_u32(fields[4].ptr, fields[4].len, &out->correct_total) ||
                !parse_u32(fields[5].ptr, fields[5].len, &expected_records)) {
                return LS_ERR_FORMAT;
            }
            if (expected_records > LS_PROGRESS_RECORDS_MAX ||
                out->correct_total > out->answered_total) {
                return LS_ERR_RANGE;
            }
            header_seen = true;
            crc = update_crc(crc, line, line_len + 1);
            pos += line_len + 1;
            continue;
        }

        if (fields[0].len == 3 && memcmp(fields[0].ptr, "END", 3) == 0) {
            uint32_t expected_crc;
            if (count != 2 || !parse_hex32(fields[1].ptr, fields[1].len,
                                           &expected_crc)) {
                return LS_ERR_FORMAT;
            }
            if (~crc != expected_crc) return LS_ERR_CHECKSUM;
            if (pos + line_len + 1 != len) return LS_ERR_FORMAT;
            end_seen = true;
            pos = len;
            break;
        }

        if (fields[0].len != 4 || memcmp(fields[0].ptr, "WORD", 4) != 0 ||
            count != 7) {
            return LS_ERR_FORMAT;
        }
        if (out->record_count >= LS_PROGRESS_RECORDS_MAX) return LS_ERR_RANGE;

        ls_progress_record_t *record = &out->records[out->record_count];
        uint32_t box;
        uint32_t correct;
        uint32_t wrong;
        if (!parse_u32(fields[1].ptr, fields[1].len, &record->word_id) ||
            !parse_u32(fields[2].ptr, fields[2].len, &box) ||
            !parse_u32(fields[3].ptr, fields[3].len, &correct) ||
            !parse_u32(fields[4].ptr, fields[4].len, &wrong) ||
            !parse_u32(fields[5].ptr, fields[5].len, &record->revision) ||
            !parse_u32(fields[6].ptr, fields[6].len, &record->due_session)) {
            return LS_ERR_FORMAT;
        }
        if (record->word_id == 0 || box > 4 ||
            correct > UINT16_MAX || wrong > UINT16_MAX) {
            return LS_ERR_RANGE;
        }
        record->box = (uint8_t)box;
        record->correct = (uint16_t)correct;
        record->wrong = (uint16_t)wrong;
        out->record_count++;
        crc = update_crc(crc, line, line_len + 1);
        pos += line_len + 1;
    }

    if (!header_seen || !end_seen) return LS_ERR_FORMAT;
    if (out->record_count != expected_records) return LS_ERR_RANGE;
    return progress_valid(out) ? LS_OK : LS_ERR_FORMAT;
}

ls_status_t ls_progress_write(const ls_progress_t *progress,
                              char *out, size_t cap, size_t *out_len)
{
    if (!progress || !out || !out_len) return LS_ERR_ARG;
    *out_len = 0;
    if (!progress_valid(progress)) return LS_ERR_FORMAT;

    char line[LINE_MAX_BYTES];
    size_t used = 0;
    uint32_t crc = 0xFFFFFFFFu;

    int written = snprintf(line, sizeof(line),
                           "LSPROG|%d|%lu|%lu|%lu|%u\n",
                           LS_PROGRESS_VERSION,
                           (unsigned long)progress->session_count,
                           (unsigned long)progress->answered_total,
                           (unsigned long)progress->correct_total,
                           (unsigned)progress->record_count);
    if (written < 0 || (size_t)written >= sizeof(line)) return LS_ERR_SIZE;
    size_t len = (size_t)written;

#define APPEND_LINE()                                                        \
    do {                                                                     \
        if (used + len > cap) return LS_ERR_SIZE;                            \
        memcpy(out + used, line, len);                                       \
        used += len;                                                         \
        crc = update_crc(crc, line, len);                                    \
    } while (0)

    APPEND_LINE();

    for (uint16_t i = 0; i < progress->record_count; i++) {
        const ls_progress_record_t *record = &progress->records[i];
        written = snprintf(line, sizeof(line), "WORD|%lu|%u|%u|%u|%lu|%lu\n",
                           (unsigned long)record->word_id,
                           (unsigned)record->box,
                           (unsigned)record->correct,
                           (unsigned)record->wrong,
                           (unsigned long)record->revision,
                           (unsigned long)record->due_session);
        if (written < 0 || (size_t)written >= sizeof(line)) return LS_ERR_SIZE;
        len = (size_t)written;
        APPEND_LINE();
    }

    written = snprintf(line, sizeof(line), "END|%08lX\n",
                       (unsigned long)~crc);
    if (written < 0 || (size_t)written >= sizeof(line)) return LS_ERR_SIZE;
    len = (size_t)written;
    if (used + len > cap) return LS_ERR_SIZE;
    memcpy(out + used, line, len);
    used += len;
    *out_len = used;
    return LS_OK;

#undef APPEND_LINE
}

static size_t pack_word_count(const ls_pack_t *pack)
{
    if (!pack) return 0;
    return pack->word_count <= LS_WORDS_MAX ? pack->word_count : LS_WORDS_MAX;
}

static bool words_have_id(const ls_word_t *words, uint16_t count, uint32_t id)
{
    if (!words) return false;
    for (uint16_t i = 0; i < count; i++) {
        if (words[i].id == id) return true;
    }
    return false;
}

static const ls_word_t *words_find_text(const ls_word_t *words,
                                        uint16_t count, const char *text)
{
    if (!words || !text) return NULL;
    for (uint16_t i = 0; i < count; i++) {
        if (ls_text_equals_ci(words[i].text, text)) return &words[i];
    }
    return NULL;
}

bool ls_progress_merge(ls_progress_t *local, const ls_progress_t *remote)
{
    if (!local || !remote) return false;
    bool changed = false;
    uint32_t local_sessions = local->session_count;

    for (uint16_t i = 0; i < remote->record_count; i++) {
        const ls_progress_record_t *incoming = &remote->records[i];
        ls_progress_record_t *existing = ls_progress_find(local, incoming->word_id);
        if (existing) {
            if (incoming->revision > existing->revision) {
                *existing = *incoming;
                changed = true;
            }
        } else if (local->record_count < LS_PROGRESS_RECORDS_MAX) {
            local->records[local->record_count++] = *incoming;
            changed = true;
        }
    }

    if (remote->session_count > local_sessions) {
        local->session_count = remote->session_count;
        local->answered_total = remote->answered_total;
        local->correct_total = remote->correct_total;
        changed = true;
    } else {
        local->session_count = local_sessions;
        if (local->correct_total > local->answered_total) {
            local->correct_total = local->answered_total;
        }
    }
    return changed;
}

size_t ls_progress_prune_words(ls_progress_t *progress, const ls_word_t *words,
                               uint16_t count)
{
    if (!progress) return 0;
    size_t removed = 0;
    while (progress->record_count >= LS_PROGRESS_RECORDS_MAX) {
        size_t victim = LS_PROGRESS_RECORDS_MAX;
        uint32_t victim_revision = UINT32_MAX;
        for (uint16_t i = 0; i < progress->record_count; i++) {
            const ls_progress_record_t *record = &progress->records[i];
            if (words_have_id(words, count, record->word_id)) continue;
            if (record->revision <= victim_revision) {
                victim_revision = record->revision;
                victim = i;
            }
        }
        if (victim == LS_PROGRESS_RECORDS_MAX) break;
        memmove(&progress->records[victim], &progress->records[victim + 1],
                (size_t)(progress->record_count - victim - 1) *
                    sizeof(progress->records[0]));
        progress->record_count--;
        removed++;
    }
    return removed;
}

size_t ls_progress_prune(ls_progress_t *progress, const ls_pack_t *pack)
{
    if (!pack) return 0;
    return ls_progress_prune_words(progress, pack->words,
                                   (uint16_t)pack_word_count(pack));
}

size_t ls_progress_remap_words(ls_progress_t *progress,
                               const ls_word_t *old_words, uint16_t old_count,
                               const ls_word_t *new_words, uint16_t new_count)
{
    if (!progress || !old_words || !new_words) return 0;
    size_t copied = 0;

    for (uint16_t i = 0; i < new_count; i++) {
        const ls_word_t *new_word = &new_words[i];
        const ls_word_t *old_word =
            words_find_text(old_words, old_count, new_word->text);
        if (!old_word || old_word->id == new_word->id) continue;
        if (ls_progress_find(progress, new_word->id)) continue;

        const ls_progress_record_t *old_record =
            ls_progress_find_const(progress, old_word->id);
        if (!old_record) continue;

        // Copy before prune/touch: both may memmove the table and invalidate
        // the pointer we just looked up.
        ls_progress_record_t snapshot = *old_record;

        if (progress->record_count >= LS_PROGRESS_RECORDS_MAX) {
            ls_progress_prune_words(progress, new_words, new_count);
        }
        ls_progress_record_t *record = ls_progress_touch(progress, new_word->id);
        if (!record) break;
        *record = snapshot;
        record->word_id = new_word->id;
        copied++;
    }
    return copied;
}

size_t ls_progress_remap_text(ls_progress_t *progress,
                              const ls_pack_t *old_pack,
                              const ls_pack_t *new_pack)
{
    if (!old_pack || !new_pack) return 0;
    return ls_progress_remap_words(
        progress, old_pack->words, (uint16_t)pack_word_count(old_pack),
        new_pack->words, (uint16_t)pack_word_count(new_pack));
}

size_t ls_progress_drop_missing_words(ls_progress_t *progress,
                                      const ls_word_t *words, uint16_t count)
{
    if (!progress) return 0;
    size_t removed = 0;
    uint16_t write = 0;
    for (uint16_t read = 0; read < progress->record_count; read++) {
        if (!words_have_id(words, count, progress->records[read].word_id)) {
            removed++;
            continue;
        }
        if (write != read) progress->records[write] = progress->records[read];
        write++;
    }
    progress->record_count = write;
    return removed;
}

size_t ls_progress_drop_missing(ls_progress_t *progress, const ls_pack_t *pack)
{
    if (!pack) return 0;
    return ls_progress_drop_missing_words(progress, pack->words,
                                          (uint16_t)pack_word_count(pack));
}
