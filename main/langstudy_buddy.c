// main/langstudy_buddy.c
// Growth, condition and wire-format logic for the digital study buddy.
#include "langstudy_buddy.h"

#include "langstudy_content.h"   // ls_crc32

#include <stdio.h>
#include <string.h>

#define LS_BUDDY_FORMAT_VERSION 1
#define LS_BUDDY_LINE_MAX 96

static const char *const s_names[LS_BUDDY_NAME_COUNT] = {
    "PIP", "MOMO", "NIB", "KIKI", "BUBO", "TARO", "YUZU", "PIXEL",
};

static const char *const s_species[LS_BUDDY_SPECIES_COUNT] = {
    "BLOB", "KIT", "PIPLING", "DRAKE",
};

// Stage 0 has no skill. Each evolution unlocks one; the descriptions are what
// the BUDDY screen shows, and the effects live in ls_buddy_award().
static const char *const s_skills[LS_BUDDY_STAGE_COUNT] = {
    "-", "FOCUS", "ECHO", "INSIGHT",
};

static const char *const s_skill_detail[LS_BUDDY_STAGE_COUNT] = {
    "STUDY TO GROW",
    "+10% XP",
    "SPEAKS AFTER A MISS",
    "+20% XP",
};

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

// ---------------------------------------------------------------------------
// Profile identity
// ---------------------------------------------------------------------------

void ls_profile_format(const uint32_t bits[4], char out[LS_PROFILE_ID_MAX + 1])
{
    if (!out) return;
    if (!bits) {
        out[0] = '\0';
        return;
    }
    // 8-4-4-4-12, lowercase, version 4 and RFC 4122 variant bits forced in.
    snprintf(out, LS_PROFILE_ID_MAX + 1,
             "%08x-%04x-4%03x-%04x-%04x%08x",
             (unsigned)bits[0],
             (unsigned)((bits[1] >> 16) & 0xFFFFu),
             (unsigned)(bits[1] & 0x0FFFu),
             (unsigned)(0x8000u | ((bits[2] >> 16) & 0x3FFFu)),
             (unsigned)(bits[2] & 0xFFFFu),
             (unsigned)bits[3]);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    // Uppercase is rejected: the id is a blob path segment and one canonical
    // spelling avoids two folders for the same profile.
    return -1;
}

bool ls_profile_valid(const char *id)
{
    if (!id || strlen(id) != LS_PROFILE_ID_MAX) return false;
    for (size_t i = 0; i < LS_PROFILE_ID_MAX; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (hex_value(id[i]) < 0) {
            return false;
        }
    }
    if (id[14] != '4') return false;
    const char variant = id[19];
    return variant == '8' || variant == '9' ||
           variant == 'a' || variant == 'b';
}

// ---------------------------------------------------------------------------
// Buddy state
// ---------------------------------------------------------------------------

const char *ls_buddy_name(uint8_t name_id)
{
    return name_id < LS_BUDDY_NAME_COUNT ? s_names[name_id] : "?";
}

const char *ls_buddy_species_name(uint8_t species)
{
    return species < LS_BUDDY_SPECIES_COUNT ? s_species[species] : "?";
}

const char *ls_buddy_skill_name(uint8_t stage)
{
    return stage < LS_BUDDY_STAGE_COUNT ? s_skills[stage] : "-";
}

const char *ls_buddy_skill_detail(uint8_t stage)
{
    return stage < LS_BUDDY_STAGE_COUNT ? s_skill_detail[stage] : "";
}

void ls_buddy_init(ls_buddy_t *buddy, uint8_t species, uint8_t name_id)
{
    if (!buddy) return;
    memset(buddy, 0, sizeof(*buddy));
    buddy->species = species < LS_BUDDY_SPECIES_COUNT ? species : 0;
    buddy->name_id = name_id < LS_BUDDY_NAME_COUNT ? name_id : 0;
    buddy->stage = 0;
    buddy->energy = 80;
    buddy->snacks = 2;
    buddy->xp = 0;
    buddy->revision = 1;
}

uint8_t ls_buddy_stage_for_xp(uint32_t xp)
{
    if (xp >= LS_BUDDY_XP_STAGE3) return 3;
    if (xp >= LS_BUDDY_XP_STAGE2) return 2;
    if (xp >= LS_BUDDY_XP_STAGE1) return 1;
    return 0;
}

uint32_t ls_buddy_next_stage_xp(uint8_t stage)
{
    switch (stage) {
    case 0: return LS_BUDDY_XP_STAGE1;
    case 1: return LS_BUDDY_XP_STAGE2;
    case 2: return LS_BUDDY_XP_STAGE3;
    default: return 0;
    }
}

bool ls_buddy_is_happy(const ls_buddy_t *buddy)
{
    return buddy && buddy->energy >= LS_BUDDY_HAPPY_ENERGY;
}

// Re-derive the stage from XP; report whether this is a new evolution.
static bool update_stage(ls_buddy_t *buddy)
{
    uint8_t next = ls_buddy_stage_for_xp(buddy->xp);
    if (next > LS_BUDDY_STAGE_COUNT - 1) next = LS_BUDDY_STAGE_COUNT - 1;
    if (next == buddy->stage) return false;
    buddy->stage = next;
    return true;
}

uint32_t ls_buddy_award(ls_buddy_t *buddy, uint8_t level, bool correct,
                        bool *evolved)
{
    if (evolved) *evolved = false;
    if (!buddy) return 0;

    if (level < 1) level = 1;
    if (level > 10) level = 10;

    uint32_t gained = 0;
    if (correct) {
        // Base XP scales with the English level, so advanced words are worth
        // more. Skills unlock multipliers; a happy buddy adds another 25%.
        uint32_t permille = 1000;
        if (buddy->stage >= 1) permille += 100;   // FOCUS
        if (buddy->stage >= 3) permille += 200;   // INSIGHT
        if (ls_buddy_is_happy(buddy)) permille = permille * 125 / 100;
        gained = (uint32_t)LS_BUDDY_XP_PER_LEVEL * level * permille / 1000;
        buddy->xp += gained;
        buddy->energy = (uint16_t)min_u32(
            (uint32_t)buddy->energy + 2, LS_BUDDY_ENERGY_MAX);
    } else if (buddy->energy >= 2) {
        buddy->energy -= 2;
    } else {
        buddy->energy = 0;
    }

    bool grew = update_stage(buddy);
    if (grew && evolved) *evolved = true;
    buddy->revision++;
    return gained;
}

void ls_buddy_finish_session(ls_buddy_t *buddy, uint16_t correct,
                             uint16_t total, bool *snack_earned, bool *evolved)
{
    if (snack_earned) *snack_earned = false;
    if (evolved) *evolved = false;
    if (!buddy) return;

    // Activity drains condition slowly, so a buddy only stays happy while the
    // learner keeps practising. Feeding restores most of one session's drain.
    buddy->energy = buddy->energy > 5 ? (uint16_t)(buddy->energy - 5) : 0;

    if (total > 0 && (uint32_t)correct * 100 >= (uint32_t)total * 70 &&
        buddy->snacks < LS_BUDDY_SNACKS_MAX) {
        buddy->snacks++;
        if (snack_earned) *snack_earned = true;
    }

    if (update_stage(buddy) && evolved) *evolved = true;
    buddy->revision++;
}

ls_buddy_feed_t ls_buddy_feed(ls_buddy_t *buddy)
{
    if (!buddy) return LS_BUDDY_NO_SNACK;
    if (buddy->energy >= LS_BUDDY_ENERGY_MAX) return LS_BUDDY_ALREADY_FULL;
    if (buddy->snacks == 0) return LS_BUDDY_NO_SNACK;

    buddy->snacks--;
    buddy->energy = (uint16_t)min_u32((uint32_t)buddy->energy + 25,
                                      LS_BUDDY_ENERGY_MAX);
    buddy->revision++;
    return LS_BUDDY_FED;
}

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------

#define LS_BUDDY_FIELDS 9

static size_t split_fields(const char *line, size_t len, const char **ptr,
                           size_t *flen, size_t max_fields)
{
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || line[i] == '|') {
            if (count >= max_fields) return 0;
            ptr[count] = line + start;
            flen[count] = i - start;
            count++;
            start = i + 1;
        }
    }
    return count;
}

static bool parse_u32(const char *text, size_t len, uint32_t max,
                      uint32_t *out)
{
    if (len == 0 || len > 10) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
        uint32_t digit = (uint32_t)(text[i] - '0');
        if (value > (max - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value > max) return false;
    *out = value;
    return true;
}

// The END line carries the CRC in the same 8-digit hex spelling the pack and
// progress formats use, so it cannot go through parse_u32().
static bool parse_hex32(const char *text, size_t len, uint32_t *out)
{
    if (len != 8) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        char c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

static bool field_is(const char *ptr, size_t len, const char *text)
{
    return strlen(text) == len && memcmp(ptr, text, len) == 0;
}

ls_buddy_status_t ls_buddy_write(const ls_buddy_t *buddy, char *out,
                                 size_t cap, size_t *out_len)
{
    if (!buddy || !out || !out_len) return LS_BUDDY_ERR_ARG;
    *out_len = 0;
    if (buddy->species >= LS_BUDDY_SPECIES_COUNT ||
        buddy->name_id >= LS_BUDDY_NAME_COUNT ||
        buddy->stage >= LS_BUDDY_STAGE_COUNT ||
        buddy->energy > LS_BUDDY_ENERGY_MAX ||
        buddy->snacks > LS_BUDDY_SNACKS_MAX) {
        return LS_BUDDY_ERR_RANGE;
    }

    char line[LS_BUDDY_LINE_MAX];
    int written = snprintf(line, sizeof(line),
                           "BUDDY|%u|%u|%u|%u|%u|%u|%lu|%lu\n",
                           LS_BUDDY_FORMAT_VERSION,
                           (unsigned)buddy->species,
                           (unsigned)buddy->name_id,
                           (unsigned)buddy->stage,
                           (unsigned)buddy->energy,
                           (unsigned)buddy->snacks,
                           (unsigned long)buddy->xp,
                           (unsigned long)buddy->revision);
    if (written < 0 || (size_t)written >= sizeof(line)) return LS_BUDDY_ERR_SIZE;

    int total = snprintf(out, cap, "%sEND|%08lX\n", line,
                         (unsigned long)ls_crc32(line, (size_t)written));
    if (total < 0 || (size_t)total >= cap) return LS_BUDDY_ERR_SIZE;
    *out_len = (size_t)total;
    return LS_BUDDY_OK;
}

ls_buddy_status_t ls_buddy_parse(const char *text, size_t len,
                                 ls_buddy_t *out)
{
    if (!text || !out) return LS_BUDDY_ERR_ARG;
    if (len == 0 || len > LS_BUDDY_TEXT_MAX) return LS_BUDDY_ERR_SIZE;
    if (text[len - 1] != '\n' || memchr(text, '\0', len)) {
        return LS_BUDDY_ERR_FORMAT;
    }

    const char *newline = memchr(text, '\n', len);
    if (!newline) return LS_BUDDY_ERR_FORMAT;
    size_t line_len = (size_t)(newline - text);
    size_t rest_len = len - line_len - 1;
    if (rest_len < 6) return LS_BUDDY_ERR_FORMAT;

    const char *ptr[LS_BUDDY_FIELDS];
    size_t flen[LS_BUDDY_FIELDS];
    size_t count = split_fields(text, line_len, ptr, flen, LS_BUDDY_FIELDS);
    uint32_t version, species, name_id, stage, energy, snacks, xp, revision;
    if (count != LS_BUDDY_FIELDS ||
        !field_is(ptr[0], flen[0], "BUDDY") ||
        !parse_u32(ptr[1], flen[1], 0xFFFFFFFFu, &version) ||
        version != LS_BUDDY_FORMAT_VERSION ||
        !parse_u32(ptr[2], flen[2], 255, &species) ||
        !parse_u32(ptr[3], flen[3], 255, &name_id) ||
        !parse_u32(ptr[4], flen[4], 255, &stage) ||
        !parse_u32(ptr[5], flen[5], 65535, &energy) ||
        !parse_u32(ptr[6], flen[6], 65535, &snacks) ||
        !parse_u32(ptr[7], flen[7], 0xFFFFFFFFu, &xp) ||
        !parse_u32(ptr[8], flen[8], 0xFFFFFFFFu, &revision)) {
        return LS_BUDDY_ERR_FORMAT;
    }
    if (species >= LS_BUDDY_SPECIES_COUNT || name_id >= LS_BUDDY_NAME_COUNT ||
        stage >= LS_BUDDY_STAGE_COUNT || energy > LS_BUDDY_ENERGY_MAX ||
        snacks > LS_BUDDY_SNACKS_MAX) {
        return LS_BUDDY_ERR_RANGE;
    }

    const char *end = text + line_len + 1;
    const char *end_ptr[2];
    size_t end_len[2];
    size_t end_count = split_fields(end, rest_len - 1, end_ptr, end_len, 2);
    uint32_t crc = 0;
    if (end_count != 2 || !field_is(end_ptr[0], end_len[0], "END") ||
        !parse_hex32(end_ptr[1], end_len[1], &crc)) {
        return LS_BUDDY_ERR_FORMAT;
    }
    if (crc != ls_crc32(text, line_len + 1)) return LS_BUDDY_ERR_FORMAT;

    out->species = (uint8_t)species;
    out->name_id = (uint8_t)name_id;
    out->stage = (uint8_t)stage;
    out->energy = (uint16_t)energy;
    out->snacks = (uint16_t)snacks;
    out->xp = xp;
    out->revision = revision;
    return LS_BUDDY_OK;
}

bool ls_buddy_merge(ls_buddy_t *local, const ls_buddy_t *remote)
{
    if (!local || !remote) return false;
    if (remote->revision <= local->revision) return false;
    *local = *remote;
    return true;
}
