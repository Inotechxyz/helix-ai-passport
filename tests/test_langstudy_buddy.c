// tests/test_langstudy_buddy.c
// Host tests for the digital buddy: profile identity, XP/skill bonuses,
// evolution thresholds, feeding, and the BUDDY wire format.
#include "langstudy_buddy.h"

#include "langstudy_content.h"   // ls_crc32, for the version 1 record

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

static void test_profile_format(void)
{
    const uint32_t bits[4] = {0x12345678u, 0xabcdef01u, 0xdeadbeefu,
                              0x0badf00du};
    char id[LS_PROFILE_ID_MAX + 1];
    ls_profile_format(bits, id);

    CHECK(strcmp(id, "12345678-abcd-4f01-9ead-beef0badf00d") == 0);
    CHECK(strlen(id) == LS_PROFILE_ID_MAX);
    CHECK(ls_profile_valid(id));

    // Uppercase is not a second spelling: it would create a second blob folder.
    char upper[LS_PROFILE_ID_MAX + 1];
    memcpy(upper, id, sizeof(upper));
    upper[0] = 'A';
    CHECK(!ls_profile_valid(upper));

    // Version nibble must stay 4 and the variant must be 8/9/a/b.
    char bad_version[LS_PROFILE_ID_MAX + 1];
    memcpy(bad_version, id, sizeof(bad_version));
    bad_version[14] = '3';
    CHECK(!ls_profile_valid(bad_version));

    char bad_variant[LS_PROFILE_ID_MAX + 1];
    memcpy(bad_variant, id, sizeof(bad_variant));
    bad_variant[19] = 'c';
    CHECK(!ls_profile_valid(bad_variant));

    CHECK(!ls_profile_valid(""));
    CHECK(!ls_profile_valid(NULL));

    char short_id[LS_PROFILE_ID_MAX + 1];
    memcpy(short_id, id, sizeof(short_id));
    short_id[LS_PROFILE_ID_MAX - 1] = '\0';
    CHECK(!ls_profile_valid(short_id));
}

static void test_names_and_stages(void)
{
    CHECK(strcmp(ls_buddy_name(0), "PIP") == 0);
    CHECK(strcmp(ls_buddy_name(LS_BUDDY_NAME_COUNT), "?") == 0);
    CHECK(ls_buddy_stage_for_xp(0) == 0);
    CHECK(ls_buddy_stage_for_xp(LS_BUDDY_XP_STAGE1 - 1) == 0);
    CHECK(ls_buddy_stage_for_xp(LS_BUDDY_XP_STAGE1) == 1);
    CHECK(ls_buddy_stage_for_xp(LS_BUDDY_XP_STAGE2) == 2);
    CHECK(ls_buddy_stage_for_xp(LS_BUDDY_XP_STAGE3) == 3);
    CHECK(ls_buddy_next_stage_xp(0) == LS_BUDDY_XP_STAGE1);
    CHECK(ls_buddy_next_stage_xp(2) == LS_BUDDY_XP_STAGE3);
    CHECK(ls_buddy_next_stage_xp(3) == 0);

    ls_buddy_t buddy;
    ls_buddy_init(&buddy, 2, 3);
    CHECK(buddy.species == 2);
    CHECK(buddy.name_id == 3);
    CHECK(buddy.stage == 0);
    CHECK(buddy.energy == 80);
    CHECK(buddy.snacks == 2);
    CHECK(buddy.xp == 0);
    CHECK(buddy.idle_epoch == 0);
    CHECK(buddy.revision == 1);
    CHECK(ls_buddy_is_happy(&buddy));

    // Out-of-range species/name fall back to the first entry.
    ls_buddy_init(&buddy, 99, 99);
    CHECK(buddy.species == 0);
    CHECK(buddy.name_id == 0);
}

static void test_xp_rewards(void)
{
    ls_buddy_t buddy;
    bool evolved = false;

    // Conditions are cleared so only the level scaling is under test.
    ls_buddy_init(&buddy, 0, 0);
    buddy.energy = 0;
    CHECK(ls_buddy_award(&buddy, 5, true, &evolved) == 50);
    CHECK(buddy.xp == 50);
    CHECK(buddy.energy == 2);
    CHECK(!evolved);
    CHECK(buddy.revision == 2);

    // Level 10 words are worth twice as much as level 5.
    buddy.energy = 0;
    CHECK(ls_buddy_award(&buddy, 10, true, &evolved) == 100);

    // FOCUS (stage 1) adds 10%.
    buddy.stage = 1;
    buddy.energy = 0;
    CHECK(ls_buddy_award(&buddy, 5, true, &evolved) == 55);

    // INSIGHT (stage 3) adds another 20%.
    buddy.stage = 3;
    buddy.energy = 0;
    CHECK(ls_buddy_award(&buddy, 5, true, &evolved) == 65);

    // A happy buddy (energy >= 60) adds 25% on the level base.
    ls_buddy_init(&buddy, 0, 0);
    CHECK(ls_buddy_award(&buddy, 5, true, &evolved) == 62);
    CHECK(buddy.energy == 82);

    // A wrong answer earns nothing and costs condition.
    uint32_t before = buddy.xp;
    CHECK(ls_buddy_award(&buddy, 5, false, &evolved) == 0);
    CHECK(buddy.xp == before);
    CHECK(buddy.energy == 80);

    // Evolution is reported once XP crosses the threshold.
    buddy.xp = LS_BUDDY_XP_STAGE1 - 5;
    buddy.stage = 0;
    buddy.energy = 0;
    CHECK(ls_buddy_award(&buddy, 1, true, &evolved) == 10);
    CHECK(evolved);
    CHECK(buddy.stage == 1);

    // An already evolved buddy never regresses, even when its persisted stage
    // is ahead of what the current XP table would derive (for example after a
    // firmware update raises the thresholds).
    buddy.xp = LS_BUDDY_XP_STAGE1 + 30;
    buddy.stage = 3;
    buddy.energy = 0;
    evolved = true;
    (void)ls_buddy_award(&buddy, 1, true, &evolved);
    CHECK(buddy.stage == 3);
    CHECK(!evolved);

    // The session end path uses the same monotonic rule.
    buddy.xp = LS_BUDDY_XP_STAGE1 + 30;
    buddy.stage = 2;
    evolved = true;
    ls_buddy_finish_session(&buddy, 0, 1, NULL, &evolved);
    CHECK(buddy.stage == 2);
    CHECK(!evolved);
}

static void test_session_and_feeding(void)
{
    ls_buddy_t buddy;
    bool snack = false;
    bool evolved = false;

    ls_buddy_init(&buddy, 0, 0);
    ls_buddy_finish_session(&buddy, 7, 10, &snack, &evolved);
    CHECK(snack);
    CHECK(buddy.snacks == 3);
    CHECK(buddy.energy == 75);
    CHECK(!evolved);

    // 70% is the boundary: 6/10 earns no snack.
    ls_buddy_init(&buddy, 0, 0);
    ls_buddy_finish_session(&buddy, 6, 10, &snack, &evolved);
    CHECK(!snack);
    CHECK(buddy.snacks == 2);

    // Energy never goes negative.
    buddy.energy = 3;
    ls_buddy_finish_session(&buddy, 0, 0, &snack, &evolved);
    CHECK(buddy.energy == 0);
    CHECK(!snack);

    // Feeding consumes one snack and restores 25 energy.
    ls_buddy_init(&buddy, 0, 0);
    buddy.energy = 50;
    CHECK(ls_buddy_feed(&buddy) == LS_BUDDY_FED);
    CHECK(buddy.energy == 75);
    CHECK(buddy.snacks == 1);

    buddy.energy = 90;
    CHECK(ls_buddy_feed(&buddy) == LS_BUDDY_FED);
    CHECK(buddy.energy == LS_BUDDY_ENERGY_MAX);

    CHECK(ls_buddy_feed(&buddy) == LS_BUDDY_ALREADY_FULL);

    buddy.energy = 10;
    buddy.snacks = 0;
    CHECK(ls_buddy_feed(&buddy) == LS_BUDDY_NO_SNACK);
    CHECK(buddy.energy == 10);
}

static void test_wire_format(void)
{
    ls_buddy_t buddy;
    ls_buddy_init(&buddy, 3, 5);
    buddy.stage = 2;
    buddy.energy = 55;
    buddy.snacks = 7;
    buddy.xp = 1234;
    buddy.idle_epoch = 1700000000u;
    buddy.revision = 9;

    char text[LS_BUDDY_TEXT_MAX];
    size_t len = 0;
    CHECK(ls_buddy_write(&buddy, text, sizeof(text), &len) == LS_BUDDY_OK);
    CHECK(len > 0 && len < sizeof(text));
    CHECK(strncmp(text, "BUDDY|2|3|5|2|55|7|1234|1700000000|9\n", 37) == 0);
    CHECK(strstr(text, "END|") != NULL);
    CHECK(text[len - 1] == '\n');

    ls_buddy_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    CHECK(ls_buddy_parse(text, len, &parsed) == LS_BUDDY_OK);
    CHECK(parsed.species == buddy.species);
    CHECK(parsed.name_id == buddy.name_id);
    CHECK(parsed.stage == buddy.stage);
    CHECK(parsed.energy == buddy.energy);
    CHECK(parsed.snacks == buddy.snacks);
    CHECK(parsed.xp == buddy.xp);
    CHECK(parsed.idle_epoch == buddy.idle_epoch);
    CHECK(parsed.revision == buddy.revision);

    // A version 1 record (written before decay existed) still loads; the
    // missing idle reference is reconstructed on the first settlement.
    char v1line[64];
    int v1line_len = snprintf(v1line, sizeof(v1line),
                              "BUDDY|1|3|5|2|55|7|1234|9\n");
    char v1[LS_BUDDY_TEXT_MAX];
    int v1_len = snprintf(v1, sizeof(v1), "%sEND|%08lX\n", v1line,
                          (unsigned long)ls_crc32(v1line, (size_t)v1line_len));
    CHECK(v1_len > 0 && (size_t)v1_len < sizeof(v1));
    memset(&parsed, 0, sizeof(parsed));
    CHECK(ls_buddy_parse(v1, (size_t)v1_len, &parsed) == LS_BUDDY_OK);
    CHECK(parsed.xp == 1234);
    CHECK(parsed.idle_epoch == 0);
    CHECK(parsed.revision == 9);

    // The field count must match the declared version.
    char wrong_count[64];
    int wc_len = snprintf(wrong_count, sizeof(wrong_count),
                          "BUDDY|1|3|5|2|55|7|1234|9|10\n");
    char wc[LS_BUDDY_TEXT_MAX];
    int wc_total = snprintf(wc, sizeof(wc), "%sEND|%08lX\n", wrong_count,
                            (unsigned long)ls_crc32(wrong_count, (size_t)wc_len));
    CHECK(wc_total > 0 && (size_t)wc_total < sizeof(wc));
    CHECK(ls_buddy_parse(wc, (size_t)wc_total, &parsed) == LS_BUDDY_ERR_FORMAT);

    // A changed CRC digit must be rejected.
    char corrupt[LS_BUDDY_TEXT_MAX];
    memcpy(corrupt, text, len);
    char *crc = strstr(corrupt, "END|") + 4;
    crc[0] = crc[0] == '0' ? '1' : '0';
    CHECK(ls_buddy_parse(corrupt, len, &parsed) == LS_BUDDY_ERR_FORMAT);

    // A missing trailing newline is a truncated record.
    CHECK(ls_buddy_parse(text, len - 1, &parsed) == LS_BUDDY_ERR_FORMAT);

    // Out-of-range fields are rejected on write and on parse.
    buddy.snacks = LS_BUDDY_SNACKS_MAX + 1;
    CHECK(ls_buddy_write(&buddy, text, sizeof(text), &len) ==
          LS_BUDDY_ERR_RANGE);

    // A tight buffer reports ERR_SIZE instead of overflowing.
    buddy.snacks = 2;
    CHECK(ls_buddy_write(&buddy, text, 8, &len) == LS_BUDDY_ERR_SIZE);
}

static void test_decay(void)
{
    const uint32_t day = 86400u;
    const uint32_t now = 1700000000u;
    const uint32_t grace = LS_BUDDY_DECAY_GRACE_DAYS * day;
    ls_buddy_t buddy;

    // Inactivity inside the grace window costs nothing, and the reference is
    // left alone so the free days are not restarted or consumed.
    ls_buddy_init(&buddy, 0, 0);
    buddy.xp = 10000;
    buddy.stage = 3;
    buddy.idle_epoch = now - 2 * day;
    uint32_t rev = buddy.revision;
    CHECK(ls_buddy_apply_decay(&buddy, now) == 0);
    CHECK(buddy.xp == 10000);
    CHECK(buddy.idle_epoch == now - 2 * day);
    CHECK(buddy.revision == rev);

    // The grace boundary itself is still free.
    buddy.idle_epoch = now - grace;
    CHECK(ls_buddy_apply_decay(&buddy, now) == 0);
    CHECK(buddy.xp == 10000);

    // One day past the grace: 1% of the remaining XP, and the evolved form
    // survives even though XP now sits below the stage 3 threshold.
    buddy.idle_epoch = now - grace - day;
    CHECK(ls_buddy_apply_decay(&buddy, now) == 100);
    CHECK(buddy.xp == 9900);
    CHECK(buddy.stage == 3);
    CHECK(buddy.revision > rev);

    // Settling again at the same instant charges nothing: the reference moved
    // forward to "now - grace", so the same days cannot be deducted twice.
    CHECK(ls_buddy_apply_decay(&buddy, now) == 0);
    CHECK(buddy.xp == 9900);

    // Another two idle days: 2% of what is left.
    CHECK(ls_buddy_apply_decay(&buddy, now + 2 * day) == 198);
    CHECK(buddy.xp == 9702);

    // The bonus stays tied to the persisted stage, not to the XP table.
    bool evolved = false;
    buddy.energy = 0;
    CHECK(ls_buddy_award(&buddy, 5, true, &evolved) == 65);
    CHECK(buddy.stage == 3);

    // An unsynced clock is ignored completely: no loss, no reference change.
    ls_buddy_init(&buddy, 0, 0);
    buddy.xp = 10000;
    buddy.idle_epoch = now - 30 * day;
    CHECK(ls_buddy_apply_decay(&buddy, LS_BUDDY_EPOCH_MIN - 1) == 0);
    CHECK(buddy.xp == 10000);
    CHECK(buddy.idle_epoch == now - 30 * day);

    // A record with no reference (v1 or first run) starts the window now.
    buddy.idle_epoch = 0;
    rev = buddy.revision;
    CHECK(ls_buddy_apply_decay(&buddy, now) == 0);
    CHECK(buddy.idle_epoch == now);
    CHECK(buddy.revision == rev + 1);

    // A clock that jumped backwards never grows XP; it just re-anchors.
    buddy.idle_epoch = now + 5 * day;
    CHECK(ls_buddy_apply_decay(&buddy, now) == 0);
    CHECK(buddy.xp == 10000);
    CHECK(buddy.idle_epoch == now);

    // Empty XP has nothing to lose, and the reference still advances.
    buddy.xp = 0;
    buddy.idle_epoch = now - 10 * day;
    CHECK(ls_buddy_apply_decay(&buddy, now) == 0);
    CHECK(buddy.xp == 0);
    CHECK(buddy.idle_epoch == now - grace);

    // Study activity restarts the idle window; a stale clock never erases it.
    ls_buddy_touch(&buddy, now);
    CHECK(buddy.idle_epoch == now);
    ls_buddy_touch(&buddy, 123u);
    CHECK(buddy.idle_epoch == now);
}

static void test_merge(void)
{
    ls_buddy_t local;
    ls_buddy_t remote;
    ls_buddy_init(&local, 0, 0);
    ls_buddy_init(&remote, 1, 1);
    local.revision = 4;
    remote.revision = 5;

    CHECK(ls_buddy_merge(&local, &remote));
    CHECK(local.species == 1);
    CHECK(local.revision == 5);

    // An equal or older remote record never wins.
    remote.revision = 5;
    remote.xp = 999;
    CHECK(!ls_buddy_merge(&local, &remote));
    CHECK(local.xp != 999);
}

int main(void)
{
    test_profile_format();
    test_names_and_stages();
    test_xp_rewards();
    test_session_and_feeding();
    test_wire_format();
    test_decay();
    test_merge();

    if (s_failures) {
        fprintf(stderr, "%d buddy test(s) failed\n", s_failures);
        return 1;
    }
    printf("langstudy buddy tests: PASS\n");
    return 0;
}
