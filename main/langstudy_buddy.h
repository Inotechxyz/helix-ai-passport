// main/langstudy_buddy.h
// Digital study buddy: identity, growth, condition and the local wire format.
//
// Pure C on purpose: the UI, storage and sync layers own all ESP-IDF calls so
// the growth maths can be exercised by the host tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RFC 4122 UUID text length without the terminator.
#define LS_PROFILE_ID_MAX 36
#define LS_BUDDY_TEXT_MAX 192

#define LS_BUDDY_SPECIES_COUNT 4
#define LS_BUDDY_NAME_COUNT 8
// Stage 0 is the creature as created; three evolutions take it to stage 3.
#define LS_BUDDY_STAGE_COUNT 4

#define LS_BUDDY_ENERGY_MAX 100
#define LS_BUDDY_SNACKS_MAX 999
#define LS_BUDDY_HAPPY_ENERGY 60

// Lifetime XP needed to reach each stage. Stage 3 is the final form. At the
// default level a happy buddy earns roughly 50 XP per correct answer, so these
// are about 3, 11 and 26 sessions (20 cards each) - deliberately challenging.
#define LS_BUDDY_XP_STAGE1 2000
#define LS_BUDDY_XP_STAGE2 8000
#define LS_BUDDY_XP_STAGE3 20000

#define LS_BUDDY_XP_PER_LEVEL 10

typedef struct {
    uint8_t species;    // 0..LS_BUDDY_SPECIES_COUNT-1
    uint8_t name_id;    // 0..LS_BUDDY_NAME_COUNT-1
    uint8_t stage;      // 0..LS_BUDDY_STAGE_COUNT-1
    uint16_t energy;    // 0..LS_BUDDY_ENERGY_MAX
    uint16_t snacks;    // feed currency earned by good sessions
    uint32_t xp;        // lifetime experience
    uint32_t revision;  // monotonic; higher wins when merging remote state
} ls_buddy_t;

typedef enum {
    LS_BUDDY_FED = 0,
    LS_BUDDY_ALREADY_FULL,
    LS_BUDDY_NO_SNACK,
} ls_buddy_feed_t;

// ---------------------------------------------------------------------------
// Profile identity
// ---------------------------------------------------------------------------

// Format four random 32-bit words as a lowercase UUIDv4. Pure so the host
// tests can pin the format; the caller fills `bits` from esp_random().
void ls_profile_format(const uint32_t bits[4], char out[LS_PROFILE_ID_MAX + 1]);

// True for exactly the lowercase 8-4-4-4-12 form with a valid version/variant.
bool ls_profile_valid(const char *id);

// ---------------------------------------------------------------------------
// Buddy state
// ---------------------------------------------------------------------------

const char *ls_buddy_name(uint8_t name_id);
const char *ls_buddy_species_name(uint8_t species);
// Skill unlocked by reaching `stage` (stage 0 has none).
const char *ls_buddy_skill_name(uint8_t stage);
const char *ls_buddy_skill_detail(uint8_t stage);

void ls_buddy_init(ls_buddy_t *buddy, uint8_t species, uint8_t name_id);

uint8_t ls_buddy_stage_for_xp(uint32_t xp);
uint32_t ls_buddy_next_stage_xp(uint8_t stage);
bool ls_buddy_is_happy(const ls_buddy_t *buddy);

// Apply one answer. Correct answers gain 10 XP per English level, multiplied by
// the unlocked skills and by the good-condition bonus; wrong answers cost
// condition but no XP. Returns the XP gained and reports an evolution.
uint32_t ls_buddy_award(ls_buddy_t *buddy, uint8_t level, bool correct,
                        bool *evolved);

// Close a session: activity drains a little condition and a session answered at
// 70 % or better earns one snack.
void ls_buddy_finish_session(ls_buddy_t *buddy, uint16_t correct, uint16_t total,
                             bool *snack_earned, bool *evolved);

// Spend one snack for condition. Feeding is refused when the buddy is full or
// has nothing to eat.
ls_buddy_feed_t ls_buddy_feed(ls_buddy_t *buddy);

// ---------------------------------------------------------------------------
// Wire format
// BUDDY|1|<species>|<name>|<stage>|<energy>|<snacks>|<xp>|<revision>
// END|<crc32>
// ---------------------------------------------------------------------------

typedef enum {
    LS_BUDDY_OK = 0,
    LS_BUDDY_ERR_ARG,
    LS_BUDDY_ERR_FORMAT,
    LS_BUDDY_ERR_RANGE,
    LS_BUDDY_ERR_SIZE,
} ls_buddy_status_t;

ls_buddy_status_t ls_buddy_write(const ls_buddy_t *buddy, char *out,
                                 size_t cap, size_t *out_len);
ls_buddy_status_t ls_buddy_parse(const char *text, size_t len,
                                 ls_buddy_t *out);

// Higher revision wins; ties keep the local state.
bool ls_buddy_merge(ls_buddy_t *local, const ls_buddy_t *remote);
