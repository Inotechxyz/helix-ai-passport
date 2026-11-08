// tests/test_langstudy_logic.c
// Host tests for the application's pure logic: content, progress, scheduling,
// phonemes, synthesis, JSON handling, generation extraction and setup forms.
#include "langstudy_autoplay.h"
#include "langstudy_builtin.h"
#include "langstudy_content.h"
#include "langstudy_edge_token.h"
#include "langstudy_gen.h"
#include "langstudy_json.h"
#include "langstudy_model.h"
#include "langstudy_phoneme.h"
#include "langstudy_progress.h"
#include "langstudy_resample.h"
#include "langstudy_session.h"
#include "langstudy_setup_form.h"
#include "langstudy_synth.h"
#include "langstudy_ws_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

// Collects streamed request bytes so the streaming emitter can be compared
// against the fixed-size builder.
typedef struct {
    char *out;
    size_t cap;
    size_t used;
} sink_buf_t;

static bool sink_buf_put(void *ctx, const char *data, size_t len)
{
    sink_buf_t *buf = (sink_buf_t *)ctx;
    if (buf->used + len > buf->cap) return false;
    memcpy(buf->out + buf->used, data, len);
    buf->used += len;
    return true;
}

static void make_pack(ls_pack_t *pack)
{
    memset(pack, 0, sizeof(*pack));
    pack->lesson_count = 1;
    pack->lessons[0].id = 1;
    snprintf(pack->lessons[0].title, sizeof(pack->lessons[0].title), "Basics");
    pack->word_count = LS_WORD_TARGET;
    for (int i = 0; i < LS_WORD_TARGET; i++) {
        ls_word_t *word = &pack->words[i];
        word->id = 1001u + (uint32_t)i;
        word->lesson_id = 1;
        snprintf(word->text, sizeof(word->text), "word%03d", i);
        snprintf(word->pos, sizeof(word->pos), "n.");
        snprintf(word->meaning, sizeof(word->meaning), "meaning %d", i);
        snprintf(word->translation, sizeof(word->translation), "学习%d", i);
        snprintf(word->example, sizeof(word->example), "This is word %d.", i);
        snprintf(word->pron, sizeof(word->pron), "B AE T");
    }
}

static void test_phoneme(void)
{
    ls_phoneme_t phonemes[LS_PRON_TOKENS_MAX];
    uint8_t stress[LS_PRON_TOKENS_MAX];

    size_t count = ls_pron_tokenize("HH AH L 'OW", phonemes, stress,
                                    LS_PRON_TOKENS_MAX);
    CHECK(count == 4);
    CHECK(phonemes[0] == LS_PH_HH);
    CHECK(phonemes[2] == LS_PH_L);
    CHECK(phonemes[3] == LS_PH_OW);
    CHECK(stress[3] == 1);
    CHECK(ls_pron_validate("HH AH L 'OW"));
    CHECK(ls_pron_validate("hh ah l ow0"));
    CHECK(ls_pron_validate(""));
    CHECK(!ls_pron_validate("XX"));
    CHECK(!ls_pron_validate("HH  AH?"));

    ls_phoneme_t value;
    uint8_t level = 0;
    CHECK(ls_phoneme_parse("OW1", 3, &value, &level));
    CHECK(value == LS_PH_OW && level == 1);
    CHECK(ls_phoneme_parse(",IY", 3, &value, &level));
    CHECK(value == LS_PH_IY && level == 2);
    CHECK(!ls_phoneme_parse("A", 1, &value, &level));

    // IPA display. A stress mark belongs at the syllable onset, and unstressed
    // AH becomes the reduced vowel. ER is always "ər" so the panel never has to
    // draw the tiny ɚ/ɝ glyphs that read as a bar.
    char ipa[LS_IPA_TEXT_MAX];
    CHECK(ls_pron_to_ipa("HH AH L 'OW", ipa, sizeof(ipa)) > 0);
    CHECK(strcmp(ipa, "/h" "\xC9\x99" "\xCB\x88" "l" "o" "\xCA\x8A" "/") == 0);
    CHECK(ls_pron_to_ipa("K AH0 M P Y UW1 T ER0", ipa, sizeof(ipa)) > 0);
    CHECK(strcmp(ipa, "/k" "\xC9\x99" "m" "\xCB\x88" "pju" "\xCB\x90" "t"
                       "\xC9\x99r" "/") == 0);
    // "desert" and "glacier" were the reported failures: ER used to become ɚ.
    CHECK(ls_pron_to_ipa("D 'EH Z ER T", ipa, sizeof(ipa)) > 0);
    CHECK(strcmp(ipa, "/" "\xCB\x88" "d" "\xC9\x9B" "z" "\xC9\x99r" "t/") == 0);
    CHECK(ls_pron_to_ipa("G L 'EY SH ER", ipa, sizeof(ipa)) > 0);
    CHECK(strcmp(ipa, "/" "\xCB\x88" "\xC9\xA1" "lei" "\xCA\x83"
                       "\xC9\x99r/") == 0);
    CHECK(ls_pron_to_ipa("B AE1 T", ipa, sizeof(ipa)) > 0);
    CHECK(strcmp(ipa, "/" "\xCB\x88" "b" "\xC3\xA6" "t/") == 0);
    // The near-close front vowel is shown as plain "i", so the AY diphthong
    // reads as "ai" instead of a featureless bar next to it.
    CHECK(ls_pron_to_ipa("K L AY1 M AH0 T", ipa, sizeof(ipa)) > 0);
    CHECK(strcmp(ipa, "/" "\xCB\x88" "klaim" "\xC9\x99" "t/") == 0);
    CHECK(ls_pron_to_ipa("R IY0 S AY1 K AH0 L", ipa, sizeof(ipa)) > 0);
    CHECK(strcmp(ipa, "/ri" "\xCB\x90" "\xCB\x88" "s" "aik" "\xC9\x99" "l/") == 0);
    CHECK(ls_pron_to_ipa("", ipa, sizeof(ipa)) == 0 && ipa[0] == '\0');
    CHECK(ls_pron_to_ipa("XX", ipa, sizeof(ipa)) == 0 && ipa[0] == '\0');
    CHECK(ls_pron_to_ipa("B AE T", ipa, 4) == 0 && ipa[0] == '\0');

    // Model replies that use IPA instead of ARPAbet must keep a usable
    // pronunciation. These are the words reported with an empty phonetic line.
    char arpa[LS_PRON_MAX + 1];
    CHECK(ls_ipa_to_arpabet("/" "\xCB\x88" "pred" "\xCA\x92" "\xC9\x99"
                            "d" "\xC9\xAA" "s/",
                            arpa, sizeof(arpa)) > 0);
    CHECK(strcmp(arpa, "P R 'EH JH AH D IH S") == 0);
    CHECK(ls_ipa_to_arpabet("/" "\xCB\x88" "kr" "\xC9\x92" "n" "\xC9\xAA"
                            "k/", arpa, sizeof(arpa)) > 0);
    CHECK(strcmp(arpa, "K R 'AA N IH K") == 0);
    // prototype: BrE /ˈprəʊtətaɪp/
    CHECK(ls_ipa_to_arpabet("/" "\xCB\x88" "pr" "\xC9\x99" "\xCA\x8A"
                            "t" "\xC9\x99" "ta" "\xC9\xAA" "p/",
                            arpa, sizeof(arpa)) > 0);
    CHECK(strcmp(arpa, "P R 'OW T AH T AY P") == 0);
    // inequality: /ˌɪnɪˈkwɒləti/
    CHECK(ls_ipa_to_arpabet("/" "\xCB\x8C" "\xC9\xAA" "n" "\xC9\xAA"
                            "\xCB\x88" "kw" "\xC9\x92" "l" "\xC9\x99"
                            "ti/", arpa, sizeof(arpa)) > 0);
    CHECK(strcmp(arpa, ",IH N IH K W 'AA L AH T IY") == 0);
    // tenant: /ˈtenənt/
    CHECK(ls_ipa_to_arpabet("/" "\xCB\x88" "ten" "\xC9\x99" "nt/",
                            arpa, sizeof(arpa)) > 0);
    CHECK(strcmp(arpa, "T 'EH N AH N T") == 0);
    // The converter also accepts a bare transcription without slashes.
    CHECK(ls_ipa_to_arpabet("\xCB\x88" "ten" "\xC9\x99" "nt",
                            arpa, sizeof(arpa)) > 0);
    CHECK(strcmp(arpa, "T 'EH N AH N T") == 0);
    CHECK(ls_ipa_to_arpabet("", arpa, sizeof(arpa)) == 0 && arpa[0] == '\0');
    CHECK(ls_ipa_to_arpabet("///", arpa, sizeof(arpa)) == 0 &&
          arpa[0] == '\0');
}

static void test_content(void)
{
    static ls_pack_t pack;
    static ls_pack_t parsed;
    static char text[LS_PACK_TEXT_MAX];
    static char copy[LS_PACK_TEXT_MAX];
    size_t len = 0;

    make_pack(&pack);
    CHECK(ls_pack_validate(&pack));

    ls_status_t status = ls_pack_write(&pack, text, sizeof(text), &len);
    CHECK(status == LS_OK);
    CHECK(len > 0 && len < sizeof(text));
    CHECK(memcmp(text, "LSPACK|2|1|" "60\n",
                 14) == 0);
    CHECK(text[len - 1] == '\n');
    CHECK(strstr(text, "|学术|") != NULL || strstr(text, "学习") != NULL);

    status = ls_pack_parse(text, len, &parsed);
    CHECK(status == LS_OK);
    CHECK(parsed.lesson_count == 1);
    CHECK(parsed.word_count == LS_WORD_TARGET);
    CHECK(parsed.words[0].id == 1001);
    CHECK(strcmp(parsed.words[LS_WORD_TARGET - 1].text, "word059") == 0);
    CHECK(strcmp(parsed.words[5].pron, "B AE T") == 0);
    CHECK(strcmp(parsed.words[7].translation, "学习7") == 0);

    // The online worker stages a generated reply in the 60-word model. The
    // smaller parse must produce the same data before the UI copies it into the
    // active pack, so no 25 KiB allocation is needed next to the reply buffer.
    static ls_gen_pack_t staged;
    static ls_pack_t promoted;
    ls_pack_view_t view = {
        .lesson_cap = LS_LESSONS_MAX,
        .word_cap = LS_GEN_WORDS_MAX,
        .lessons = staged.lessons,
        .words = staged.words,
    };
    CHECK(ls_pack_parse_view(text, len, &view) == LS_OK);
    staged.lesson_count = view.lesson_count;
    staged.word_count = view.word_count;
    CHECK(ls_pack_validate_view(&view));
    CHECK(staged.word_count == LS_WORD_TARGET);
    ls_pack_copy_generated(&promoted, &staged);
    CHECK(ls_pack_validate(&promoted));
    CHECK(promoted.word_count == parsed.word_count);
    CHECK(strcmp(promoted.words[LS_WORD_TARGET - 1].text,
                 parsed.words[LS_WORD_TARGET - 1].text) == 0);

    // A reply declaring more words than the staging capacity is rejected
    // before any line is copied, so the caller can retry instead of overflowing.
    static char over[LS_PACK_TEXT_MAX];
    memcpy(over, text, len);
    char *count_field = strstr(over, "|" "60\n");
    CHECK(count_field != NULL);
    if (count_field) {
        count_field[1] = '6';
        count_field[2] = '1';
        CHECK(ls_pack_parse_view(over, len, &view) == LS_ERR_RANGE);
    }

    // Version 1 packs are rejected: the translation field changed the format.
    static char legacy[LS_PACK_TEXT_MAX];
    size_t legacy_len = 0;
    memcpy(legacy, text, len);
    legacy[7] = '1';
    legacy_len = len;
    CHECK(ls_pack_parse(legacy, legacy_len, &parsed) == LS_ERR_FORMAT);

    memcpy(copy, text, len);
    char *found = strstr(copy, "WORD|1001");
    CHECK(found != NULL);
    if (found) {
        found[8] = '2'; // 1001 -> 1002: still well-formed, wrong CRC
        CHECK(ls_pack_parse(copy, len, &parsed) == LS_ERR_CHECKSUM);
    }

    // The parser accepts any count up to the 100-word capacity, so the
    // built-in pack stays valid; zero and over-capacity counts do not.
    ls_pack_t invalid = pack;
    invalid.word_count = 0;
    CHECK(!ls_pack_validate(&invalid));

    invalid = pack;
    invalid.word_count = LS_WORDS_MAX + 1;
    CHECK(!ls_pack_validate(&invalid));

    invalid = pack;
    snprintf(invalid.words[1].text, sizeof(invalid.words[1].text), "word000");
    CHECK(!ls_pack_validate(&invalid));

    invalid = pack;
    snprintf(invalid.words[3].pron, sizeof(invalid.words[3].pron), "NOPE");
    CHECK(!ls_pack_validate(&invalid));

    // A pack without a Chinese translation no longer satisfies the contract.
    invalid = pack;
    snprintf(invalid.words[3].translation,
             sizeof(invalid.words[3].translation), "%s", "");
    CHECK(!ls_pack_validate(&invalid));

    invalid = pack;
    snprintf(invalid.words[3].translation,
             sizeof(invalid.words[3].translation), "%s", "english");
    CHECK(!ls_pack_validate(&invalid));

    CHECK(ls_pack_find_word(&pack, 1005) == &pack.words[4]);
    CHECK(ls_pack_find_text(&pack, "WORD005") != NULL);
    CHECK(ls_pack_find_text(&pack, "missing") == NULL);

    uint32_t id_a = ls_word_id_for_text("Hello");
    uint32_t id_b = ls_word_id_for_text("  hello  ");
    CHECK(id_a == id_b);
    CHECK((id_a & 0x80000000u) != 0);
    CHECK(id_a != ls_word_id_for_text("world"));

    ls_pack_t generated = pack;
    ls_pack_assign_generated_ids(&generated);
    CHECK(ls_pack_validate(&generated));
    CHECK((generated.words[0].id & 0x80000000u) != 0);
}

static void test_progress_and_session(void)
{
    static ls_pack_t pack;
    static ls_progress_t progress;
    static ls_progress_t parsed;
    static char text[LS_PROGRESS_TEXT_MAX];
    uint32_t ids[LS_SESSION_CARDS];
    size_t len = 0;

    make_pack(&pack);
    ls_progress_init(&progress);

    size_t count = ls_session_build(&pack, &progress, ids);
    CHECK(count == LS_SESSION_CARDS);
    CHECK(ids[0] == 1001);
    CHECK(ids[19] == 1020);

    ls_progress_answer(&progress, 1001, true);
    ls_progress_answer(&progress, 1002, false);
    ls_progress_record_t *record = ls_progress_find(&progress, 1001);
    CHECK(record != NULL);
    if (record) {
        CHECK(record->box == 1);
        CHECK(record->due_session == 3); // 0 + 2 + interval[1]
        CHECK(record->correct == 1);
        CHECK(record->revision == 1);
    }
    record = ls_progress_find(&progress, 1002);
    CHECK(record != NULL);
    if (record) {
        CHECK(record->box == 0);
        CHECK(record->due_session == 2);
        CHECK(record->wrong == 1);
    }
    CHECK(progress.answered_total == 2);
    CHECK(progress.correct_total == 1);

    ls_progress_finish_session(&progress);
    CHECK(progress.session_count == 1);

    count = ls_session_build(&pack, &progress, ids);
    CHECK(count == LS_SESSION_CARDS);
    CHECK(ids[0] == 1002); // due first
    bool has_1001 = false;
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == 1001) has_1001 = true;
    }
    CHECK(!has_1001);
    CHECK(ids[1] == 1021); // rotated new-word window

    ls_progress_answer(&progress, 1002, true);
    ls_progress_finish_session(&progress);
    count = ls_session_build(&pack, &progress, ids);
    CHECK(count == LS_SESSION_CARDS);
    CHECK(ids[0] == 1001); // due at session index 3

    ls_status_t status = ls_progress_write(&progress, text, sizeof(text), &len);
    CHECK(status == LS_OK);
    // Progress stayed at v1 when packs moved to v2, so on-device records from
    // earlier firmware versions still load.
    CHECK(memcmp(text, "LSPROG|1|", 9) == 0);
    status = ls_progress_parse(text, len, &parsed);
    CHECK(status == LS_OK);
    CHECK(parsed.session_count == progress.session_count);
    CHECK(parsed.record_count == progress.record_count);
    CHECK(parsed.answered_total == progress.answered_total);
    CHECK(ls_progress_find_const(&parsed, 1001) != NULL);

    // Merge: newer revision wins, ties keep local, larger session count wins.
    ls_progress_t remote;
    ls_progress_init(&remote);
    remote.session_count = 10;
    remote.answered_total = 20;
    remote.correct_total = 15;
    remote.record_count = 2;
    remote.records[0].word_id = 1001;
    remote.records[0].box = 4;
    remote.records[0].revision = 99;
    remote.records[1].word_id = 1003;
    remote.records[1].box = 2;
    remote.records[1].revision = 1;
    CHECK(ls_progress_merge(&progress, &remote));
    record = ls_progress_find(&progress, 1001);
    CHECK(record && record->box == 4 && record->revision == 99);
    CHECK(ls_progress_find(&progress, 1003) != NULL);
    CHECK(progress.session_count == 10);
    CHECK(progress.answered_total == 20);

    ls_progress_t tie = remote;
    tie.records[0].box = 0;
    ls_progress_record_t *before = ls_progress_find(&progress, 1001);
    uint8_t before_box = before ? before->box : 0xFF;
    ls_progress_merge(&progress, &tie);
    record = ls_progress_find(&progress, 1001);
    CHECK(record && record->box == before_box);
}

static bool ids_overlap(const uint32_t *a, size_t a_len, const uint32_t *b,
                        size_t b_len)
{
    for (size_t i = 0; i < a_len; i++) {
        for (size_t j = 0; j < b_len; j++) {
            if (a[i] == b[j]) return true;
        }
    }
    return false;
}

static void test_session_rotation(void)
{
    static ls_pack_t pack;
    static ls_progress_t progress;
    uint32_t first[LS_SESSION_CARDS];
    uint32_t second[LS_SESSION_CARDS];

    make_pack(&pack);
    ls_progress_init(&progress);

    size_t first_len = ls_session_build_ex(&pack, &progress, NULL, 0, first);
    CHECK(first_len == LS_SESSION_CARDS);

    // With 40 unused words left, the next session avoids every word from the
    // previous one.
    size_t second_len =
        ls_session_build_ex(&pack, &progress, first, first_len, second);
    CHECK(second_len == LS_SESSION_CARDS);
    CHECK(!ids_overlap(second, second_len, first, first_len));

    // A 30-word pack leaves exactly 10 words after excluding the previous
    // session: still a full minimum session with no repetition.
    pack.word_count = 30;
    size_t short_len =
        ls_session_build_ex(&pack, &progress, first, first_len, second);
    CHECK(short_len == LS_SESSION_MIN);
    CHECK(!ids_overlap(second, short_len, first, first_len));

    // A 25-word pack cannot fill 10 different words, so the builder reuses
    // previous words rather than returning a short session.
    pack.word_count = 25;
    size_t fallback_len =
        ls_session_build_ex(&pack, &progress, first, first_len, second);
    CHECK(fallback_len == LS_SESSION_CARDS);
    CHECK(ids_overlap(second, fallback_len, first, first_len));
    make_pack(&pack);
}

static void test_remap_and_prune(void)
{
    static ls_pack_t old_pack;
    static ls_pack_t new_pack;
    static ls_progress_t progress;

    make_pack(&old_pack);
    new_pack = old_pack;
    ls_pack_assign_generated_ids(&new_pack);
    CHECK(new_pack.words[0].id != old_pack.words[0].id);

    ls_progress_init(&progress);
    ls_progress_answer(&progress, old_pack.words[0].id, true);
    ls_progress_answer(&progress, old_pack.words[1].id, false);

    size_t copied = ls_progress_remap_text(&progress, &old_pack, &new_pack);
    CHECK(copied == 2);
    CHECK(ls_progress_find(&progress, new_pack.words[0].id) != NULL);
    CHECK(ls_progress_find(&progress, new_pack.words[1].id) != NULL);
    CHECK(ls_progress_find(&progress, new_pack.words[0].id)->box == 1);

    // Remap leaves the superseded IDs behind; drop_missing clears them.
    CHECK(ls_progress_find(&progress, old_pack.words[0].id) != NULL);
    size_t dropped = ls_progress_drop_missing(&progress, &new_pack);
    CHECK(dropped == 2);
    CHECK(ls_progress_find(&progress, old_pack.words[0].id) == NULL);
    CHECK(ls_progress_find(&progress, old_pack.words[1].id) == NULL);
    CHECK(progress.record_count == 2);

    // Fill the table with records that are not in the active pack.
    ls_progress_init(&progress);
    for (uint16_t i = 0; i < LS_PROGRESS_RECORDS_MAX; i++) {
        progress.records[i].word_id = 5000u + i;
        progress.records[i].revision = i;
        progress.records[i].box = 1;
    }
    progress.record_count = LS_PROGRESS_RECORDS_MAX;
    size_t removed = ls_progress_prune(&progress, &old_pack);
    CHECK(removed == 1);
    CHECK(progress.record_count == LS_PROGRESS_RECORDS_MAX - 1);
}

static void test_json_and_gen(void)
{
    char escaped[128];
    size_t escaped_len = 0;
    CHECK(ls_json_escape("a\"b\nc\\d", escaped, sizeof(escaped), &escaped_len));
    CHECK(strcmp(escaped, "a\\\"b\\nc\\\\d") == 0);

    char value[64];
    size_t value_len = 0;
    const char *small = "{\"pack\":\"hello\\nworld\"}";
    CHECK(ls_json_extract_string(small, strlen(small), "pack",
                                 value, sizeof(value), &value_len));
    CHECK(strcmp(value, "hello\nworld") == 0);
    CHECK(value_len == strlen(value));
    CHECK(!ls_json_extract_string(small, strlen(small), "missing",
                                  value, sizeof(value), &value_len));

    char prompt[LS_GEN_PROMPT_BASE_MAX + LS_AVOID_WORDS_MAX];
    size_t prompt_len = 0;
    CHECK(ls_gen_build_prompt(prompt, sizeof(prompt), &prompt_len,
                              LS_EXAM_CET6, "alpha beta gamma", false));
    CHECK(prompt_len > 100);
    CHECK(strstr(prompt, "CET-6") != NULL);
    CHECK(strstr(prompt, "alpha beta gamma") != NULL);
    CHECK(strstr(prompt, "the 3 words") != NULL);
    // The prompt must pin the reply shape tightly enough that a stray prose
    // preamble or a short pack is not a normal outcome.
    CHECK(strstr(prompt, "nothing else") != NULL);
    CHECK(strstr(prompt, "exactly 9 pipe-separated fields") != NULL);
    CHECK(strstr(prompt, "60th WORD line") != NULL);
    CHECK(strstr(prompt, "never stop early") != NULL);
    // REFRESH must not hand back the same vocabulary: the model is told to
    // sample randomly and the learned/current list is a hard exclusion.
    CHECK(strstr(prompt, "Randomly sample 60") != NULL);
    CHECK(strstr(prompt, "already learned") != NULL);

    // Worst-case avoid list must still fit the PROMPT_CAP-sized buffer used by
    // the DeepSeek worker (LS_GEN_PROMPT_BASE_MAX bytes of base prompt plus
    // the list). The strict retry text must fit in the same budget.
    char big_avoid[LS_AVOID_WORDS_MAX];
    for (size_t i = 0; i + 1 < sizeof(big_avoid); i++) big_avoid[i] = 'a';
    big_avoid[sizeof(big_avoid) - 1] = '\0';
    char full_prompt[LS_GEN_PROMPT_BASE_MAX + LS_AVOID_WORDS_MAX];
    size_t full_len = 0;
    CHECK(ls_gen_build_prompt(full_prompt, sizeof(full_prompt), &full_len,
                              LS_EXAM_CET4, big_avoid, true));
    CHECK(full_len < sizeof(full_prompt));
    // The retry after a repeated-word rejection carries an explicit warning.
    CHECK(strstr(full_prompt, "STRICT") != NULL);
    CHECK(strstr(prompt, "STRICT") == NULL);
    CHECK(strstr(prompt, "Uniqueness is enforced") != NULL);

    char ielts_prompt[LS_GEN_PROMPT_BASE_MAX + LS_AVOID_WORDS_MAX];
    size_t ielts_len = 0;
    CHECK(ls_gen_build_prompt(ielts_prompt, sizeof(ielts_prompt), &ielts_len,
                              LS_EXAM_IELTS, NULL, false));
    CHECK(strstr(ielts_prompt, "IELTS") != NULL);
    CHECK(strstr(ielts_prompt, "Do NOT reuse") == NULL);

    char request[4096];
    size_t request_len = 0;
    CHECK(ls_gen_build_request(request, sizeof(request), "deepseek-chat",
                               prompt, &request_len));
    CHECK(request_len > prompt_len);
    CHECK(strstr(request, "\"model\":\"deepseek-chat\"") != NULL);
    CHECK(strstr(request, "\"response_format\":{\"type\":\"json_object\"}") != NULL);
    // The HTTPS worker consumes SSE so a slow 60-word generation does not
    // trip the per-read socket timeout.
    CHECK(strstr(request, "\"stream\":true") != NULL);

    // The streamed body must equal the fixed-size body, and the Content-Length
    // helper must match it exactly; otherwise the header and the bytes written
    // to the socket would disagree and the request would hang or be rejected.
    char streamed[4096];
    sink_buf_t sink = {.out = streamed, .cap = sizeof(streamed), .used = 0};
    CHECK(ls_gen_request_size("deepseek-chat", prompt) == request_len);
    CHECK(ls_gen_emit_request(sink_buf_put, &sink, "deepseek-chat", prompt));
    CHECK(sink.used == request_len);
    CHECK(sink.used <= sizeof(streamed));
    CHECK(memcmp(streamed, request, request_len) == 0);

    // A sink that cannot take the whole body must fail rather than truncate.
    char tiny[8];
    sink_buf_t tiny_sink = {.out = tiny, .cap = sizeof(tiny), .used = 0};
    CHECK(!ls_gen_emit_request(sink_buf_put, &tiny_sink, "deepseek-chat",
                               prompt));

    // Build a valid pack without END, then wrap it in the two nested JSON
    // strings that the API returns.
    static ls_pack_t pack;
    make_pack(&pack);
    static char pack_text[LS_PACK_TEXT_MAX];
    size_t pack_text_len = 0;
    CHECK(ls_pack_write(&pack, pack_text, sizeof(pack_text), &pack_text_len) == LS_OK);
    char *end = strstr(pack_text, "END|");
    CHECK(end != NULL);
    if (end) {
        pack_text_len = (size_t)(end - pack_text);
        pack_text[pack_text_len] = '\0';
    }

    size_t pack_text_escaped_len = 0;
    char *pack_text_escaped = malloc(LS_PACK_TEXT_MAX * 2);
    char *inner = malloc(LS_PACK_TEXT_MAX * 2);
    char *content_escaped = malloc(LS_PACK_TEXT_MAX * 3);
    char *response = malloc(LS_PACK_TEXT_MAX * 3);
    char *pack_out = malloc(LS_PACK_TEXT_MAX);
    CHECK(pack_text_escaped && inner && content_escaped && response && pack_out);
    if (pack_text_escaped && inner && content_escaped && response && pack_out) {
        CHECK(ls_json_escape(pack_text, pack_text_escaped,
                             LS_PACK_TEXT_MAX * 2, &pack_text_escaped_len));
        snprintf(inner, LS_PACK_TEXT_MAX * 2, "{\"pack\":\"%s\"}",
                 pack_text_escaped);
        size_t content_escaped_len = 0;
        CHECK(ls_json_escape(inner, content_escaped, LS_PACK_TEXT_MAX * 3,
                             &content_escaped_len));
        snprintf(response, LS_PACK_TEXT_MAX * 3,
                 "{\"choices\":[{\"message\":{\"content\":\"%s\"}}]}",
                 content_escaped);

        size_t pack_len = 0;
        bool extracted = ls_gen_extract_pack(response, strlen(response),
                                             pack_out, LS_PACK_TEXT_MAX,
                                             &pack_len);
        CHECK(extracted);
        if (extracted) {
            static ls_pack_t parsed;
            CHECK(ls_pack_parse(pack_out, pack_len, &parsed) == LS_OK);
            CHECK(parsed.word_count == LS_WORD_TARGET);
            CHECK(strcmp(parsed.words[0].translation, "学习0") == 0);
        }

        // The streaming reader must agree with the one-shot extractor even
        // when the body arrives one byte at a time, as TLS records can.
        char *stream_out = malloc(LS_PACK_TEXT_MAX);
        CHECK(stream_out != NULL);
        if (stream_out) {
            ls_gen_reader_t reader;
            ls_gen_reader_init(&reader, stream_out, LS_PACK_TEXT_MAX);
            bool fed = true;
            size_t response_len = strlen(response);
            for (size_t i = 0; i < response_len && fed; i++) {
                fed = ls_gen_reader_feed(&reader, response + i, 1);
            }
            size_t stream_len = 0;
            CHECK(fed);
            CHECK(ls_gen_reader_finish(&reader, &stream_len));
            CHECK(stream_len == pack_len);
            CHECK(memcmp(stream_out, pack_out, pack_len) == 0);

            // A value cut off mid-string is never finalized.
            ls_gen_reader_init(&reader, stream_out, LS_PACK_TEXT_MAX);
            CHECK(ls_gen_reader_feed(&reader, response, response_len / 2));
            CHECK(!ls_gen_reader_finish(&reader, &stream_len));

            // \uXXXX escapes and surrogate pairs decode across chunks too.
            const char *unicode =
                "{\"content\":\"\\u5b66\\u4e60 \\ud83d\\ude00\"}";
            ls_gen_reader_init(&reader, stream_out, LS_PACK_TEXT_MAX);
            CHECK(ls_gen_reader_feed(&reader, unicode, strlen(unicode)));
            stream_len = 0;
            CHECK(!ls_gen_reader_finish(&reader, &stream_len));
            CHECK(strcmp(stream_out,
                         "\xe5\xad\xa6\xe4\xb9\xa0 \xf0\x9f\x98\x80") == 0);
        }
        free(stream_out);

        // Contents assembled from SSE deltas are exactly the message content;
        // the streaming finalizer must yield the same finalized pack.
        char *sse_out = malloc(LS_PACK_TEXT_MAX + 512);
        CHECK(sse_out != NULL);
        if (sse_out) {
            size_t sse_len = strlen(inner);
            memcpy(sse_out, inner, sse_len);
            CHECK(ls_gen_finalize_pack(sse_out, &sse_len,
                                       LS_PACK_TEXT_MAX + 512));
            CHECK(sse_len == pack_len);
            CHECK(memcmp(sse_out, pack_out, pack_len) == 0);
            free(sse_out);
        }
    }
    free(pack_text_escaped);
    free(inner);
    free(content_escaped);
    free(response);
    free(pack_out);
}

// A generation that stops after the 45th WORD line, or appends prose, must
// still yield a usable pack: the sanitizer trims to complete lines, fixes the
// header counts and re-finalizes. This is what keeps IELTS refreshes from
// failing outright on a short model reply.
static void test_sanitize_pack(void)
{
    static ls_pack_t pack;
    make_pack(&pack);
    static char full[LS_PACK_TEXT_MAX];
    size_t full_len = 0;
    CHECK(ls_pack_write(&pack, full, sizeof(full), &full_len) == LS_OK);
    CHECK(full_len > 0);

    // Cut mid-way through the WORD block, inside a line (no trailing newline).
    size_t cut = 0;
    size_t words_seen = 0;
    for (size_t i = 0; i < full_len; i++) {
        if (i + 5 < full_len && memcmp(full + i, "WORD|", 5) == 0) {
            words_seen++;
            if (words_seen == 46) {
                cut = i;
                break;
            }
        }
    }
    CHECK(words_seen == 46 && cut > 0);
    if (cut > 0) {
        // Split inside the 46th WORD line so it is a partial tail.
        size_t partial = (size_t)strlen("WORD|1046|1") / 2;
        cut += partial;
        static char text[LS_PACK_TEXT_MAX];
        size_t len = cut;
        memcpy(text, full, len);
        CHECK(ls_gen_sanitize_pack(text, &len, sizeof(text),
                                   LS_WORD_MIN_ACCEPT));
        CHECK(strncmp(text, "LSPACK|2|1|45\n", 14) == 0);
        static ls_pack_t parsed;
        CHECK(ls_pack_parse(text, len, &parsed) == LS_OK);
        CHECK(parsed.word_count == 45);
        CHECK(parsed.words[44].id == 1045);
    }

    // A prose preamble, trailing prose, a model-supplied END and a bogus
    // header count are all repaired in one pass.
    static char messy[LS_PACK_TEXT_MAX + 256];
    const char *full_body = strstr(full, "LESSON|");
    CHECK(full_body != NULL);
    if (full_body) {
        size_t body_len = full_len - (size_t)(full_body - full);
        // Drop the real END line so only the model's copy remains.
        const char *full_end = strstr(full, "END|");
        if (full_end) body_len = (size_t)(full_end - full_body);
        int written = snprintf(messy, sizeof(messy),
                               "Here is your pack:\nLSPACK|2|9|99\n%.*s"
                               "END|DEADBEEF\nHope this helps!\n",
                               (int)body_len, full_body);
        CHECK(written > 0);
        size_t len = (size_t)written;
        CHECK(ls_gen_sanitize_pack(messy, &len, sizeof(messy),
                                   LS_WORD_MIN_ACCEPT));
        static ls_pack_t parsed;
        CHECK(ls_pack_parse(messy, len, &parsed) == LS_OK);
        CHECK(parsed.lesson_count == 1);
        CHECK(parsed.word_count == LS_WORD_TARGET);
        CHECK(strcmp(parsed.words[0].translation, "学习0") == 0);
    }

    // Too few usable words is still a failure.
    static ls_pack_t tiny;
    memset(&tiny, 0, sizeof(tiny));
    tiny.lesson_count = 1;
    tiny.lessons[0].id = 1;
    snprintf(tiny.lessons[0].title, sizeof(tiny.lessons[0].title), "Basics");
    tiny.word_count = 3;
    for (int i = 0; i < 3; i++) {
        ls_word_t *word = &tiny.words[i];
        word->id = 2000u + (uint32_t)i;
        word->lesson_id = 1;
        snprintf(word->text, sizeof(word->text), "tiny%d", i);
        snprintf(word->pos, sizeof(word->pos), "n.");
        snprintf(word->meaning, sizeof(word->meaning), "meaning %d", i);
        snprintf(word->translation, sizeof(word->translation), "学习%d", i);
        snprintf(word->example, sizeof(word->example), "This is tiny %d.", i);
        snprintf(word->pron, sizeof(word->pron), "T AY N IY");
    }
    static char tiny_text[LS_PACK_TEXT_MAX];
    size_t tiny_len = 0;
    CHECK(ls_pack_write(&tiny, tiny_text, sizeof(tiny_text), &tiny_len) == LS_OK);
    CHECK(!ls_gen_sanitize_pack(tiny_text, &tiny_len, sizeof(tiny_text),
                                LS_WORD_MIN_ACCEPT));

    // No LSPACK header at all is rejected rather than guessed at.
    static char prose[128];
    strcpy(prose, "Sorry, I cannot generate that right now.\n");
    size_t prose_len = strlen(prose);
    CHECK(!ls_gen_sanitize_pack(prose, &prose_len, sizeof(prose),
                                LS_WORD_MIN_ACCEPT));

    // IELTS-style reply: IPA in pron, an overlong meaning, curly quotes in the
    // example, a duplicate word, an empty translation and a WORD line missing
    // its pron field. The strict parser returns LS_ERR_RANGE on the IPA alone;
    // the sanitizer repairs what it can and drops only the unusable words.
    static char model[LS_PACK_TEXT_MAX];
    size_t model_len = 0;
    int w = snprintf(model, sizeof(model), "LSPACK|2|1|60\nLESSON|1|Basics\n");
    CHECK(w > 0);
    model_len = (size_t)w;
    for (int i = 0; i < LS_WORD_TARGET && model_len < sizeof(model); i++) {
        char word[LS_WORD_TEXT_MAX + 1];
        if (i == 3) {
            snprintf(word, sizeof(word), "MODEL000");  // duplicate of word 0
        } else {
            snprintf(word, sizeof(word), "model%03d", i);
        }
        const char *meaning = "short meaning";
        char long_meaning[48];
        if (i == 1) {
            memset(long_meaning, 'a', 45);
            long_meaning[45] = '\0';
            meaning = long_meaning;
        }
        const char *translation = "学习";
        if (i == 4) translation = "";
        const char *example = "A sample sentence.";
        if (i == 2) example = "\xE2\x80\x9Cquoted\xE2\x80\x9D";
        const char *pron = "B AE T";
        if (i == 0) pron = "/\xCB\x88ten\xC9\x99nt/";

        char text[256];
        if (i == 5) {
            // Missing the pron field (8 fields): no room to repair in place.
            w = snprintf(text, sizeof(text),
                         "WORD|%d|1|%s|n.|%s|%s|%s\n", i + 1, word, meaning,
                         translation, example);
        } else {
            w = snprintf(text, sizeof(text),
                         "WORD|%d|1|%s|n.|%s|%s|%s|%s\n", i + 1, word,
                         meaning, translation, example, pron);
        }
        CHECK(w > 0);
        if (w <= 0 || (size_t)w >= sizeof(text) ||
            model_len + (size_t)w >= sizeof(model)) {
            break;
        }
        memcpy(model + model_len, text, (size_t)w);
        model_len += (size_t)w;
    }
    size_t model_sz = model_len;
    CHECK(ls_gen_sanitize_pack(model, &model_sz, sizeof(model),
                               LS_WORD_MIN_ACCEPT));
    static ls_pack_t repaired;
    CHECK(ls_pack_parse(model, model_sz, &repaired) == LS_OK);
    CHECK(repaired.word_count == LS_WORD_TARGET - 3);
    // IPA is converted, not cleared, even though the ARPAbet is a little longer.
    CHECK(strcmp(repaired.words[0].pron, "T 'EH N AH N T") == 0);
    CHECK(strlen(repaired.words[1].meaning) == LS_MEANING_MAX);
    CHECK(strcmp(repaired.words[2].example, "\"quoted\"") == 0);
}

// Auto-play pins the lesson of the word it starts on, advances on its own
// deadline, wraps at the end and never reports a word when inactive.
static void test_autoplay(void)
{
    static ls_pack_t pack;
    make_pack(&pack);
    // Add a second lesson so the loop filter has something to exclude.
    pack.lesson_count = 2;
    pack.lessons[1].id = 2;
    snprintf(pack.lessons[1].title, sizeof(pack.lessons[1].title), "Extra");
    pack.words[59].lesson_id = 2;
    pack.words[59].id = 9999;
    // Lesson 1 therefore has 59 words (1001..1059).

    ls_autoplay_t ap;
    memset(&ap, 0, sizeof(ap));
    CHECK(ls_autoplay_current_id(&ap) == 0);
    // Dwell must be non-zero and the lesson must contain words.
    CHECK(!ls_autoplay_start(&ap, &pack, 1, 1001, 0, 0));
    CHECK(!ls_autoplay_start(&ap, &pack, 77, 1001, 0, 5000));
    CHECK(!ap.active);

    int64_t now = 1000000;  // 1 s
    CHECK(ls_autoplay_start(&ap, &pack, 1, 1030, now, 5000));
    CHECK(ap.active);
    CHECK(ap.count == 59);
    CHECK(ap.index == 29);           // 1030 is the 30th lesson-1 word
    CHECK(ls_autoplay_current_id(&ap) == 1030);

    // The deadline is measured from the start, and the bar tracks it.
    CHECK(ls_autoplay_remaining_ms(&ap, now, 5000) == 5000);
    CHECK(ls_autoplay_remaining_permille(&ap, now, 5000) == 1000);
    CHECK(!ls_autoplay_poll(&ap, now + 4999999, 5000));
    CHECK(ls_autoplay_current_id(&ap) == 1030);
    CHECK(ls_autoplay_remaining_ms(&ap, now + 2500000, 5000) == 2500);
    CHECK(ls_autoplay_remaining_permille(&ap, now + 2500000, 5000) == 500);

    // A late tick advances exactly once and measures the next dwell from now.
    now += 6000000;
    CHECK(ls_autoplay_poll(&ap, now, 5000));
    CHECK(ls_autoplay_current_id(&ap) == 1031);
    CHECK(ls_autoplay_remaining_ms(&ap, now, 5000) == 5000);
    CHECK(!ls_autoplay_poll(&ap, now, 5000));

    // Wrapping from the last lesson-1 word goes back to the first.
    ap.index = (uint16_t)(ap.count - 1);
    now += 6000000;
    CHECK(ls_autoplay_poll(&ap, now, 5000));
    CHECK(ap.index == 0);
    CHECK(ls_autoplay_current_id(&ap) == 1001);

    // Lesson 2 only ever loops its own word. Toggle from idle, since toggling
    // an active loop is defined to stop it.
    ls_autoplay_stop(&ap);
    CHECK(ls_autoplay_toggle(&ap, &pack, 2, 9999, now, 5000));
    CHECK(ap.active && ap.count == 1);
    CHECK(ls_autoplay_current_id(&ap) == 9999);
    CHECK(ls_autoplay_poll(&ap, now + 6000000, 5000));
    CHECK(ls_autoplay_current_id(&ap) == 9999);

    // Toggling off clears the state.
    CHECK(!ls_autoplay_toggle(&ap, &pack, 1, 1001, now, 5000));
    CHECK(!ap.active);
    CHECK(ls_autoplay_current_id(&ap) == 0);
    CHECK(ls_autoplay_remaining_ms(&ap, now, 5000) == 0);
    CHECK(ls_autoplay_remaining_permille(&ap, now, 5000) == 0);

    // Starting on a word outside the lesson falls back to the first word.
    CHECK(ls_autoplay_start(&ap, &pack, 1, 9999, now, 5000));
    CHECK(ap.index == 0);
    CHECK(ls_autoplay_current_id(&ap) == 1001);
    ls_autoplay_stop(&ap);
    CHECK(!ap.active);
}

static void test_setup_form(void)
{
    char out[64];
    size_t len = 0;
    const char *body = "ssid=My+Net&password=p%40ss%21&api_key=sk-abc12345";
    CHECK(ls_form_get(body, strlen(body), "ssid", out, sizeof(out), &len));
    CHECK(strcmp(out, "My Net") == 0);
    CHECK(ls_form_get(body, strlen(body), "password", out, sizeof(out), &len));
    CHECK(strcmp(out, "p@ss!") == 0);
    CHECK(!ls_form_get(body, strlen(body), "missing", out, sizeof(out), &len));

    CHECK(ls_setup_validate_ssid("My Net"));
    CHECK(!ls_setup_validate_ssid(""));
    CHECK(!ls_setup_validate_ssid("has\ncontrol"));
    CHECK(ls_setup_validate_password(""));
    CHECK(ls_setup_validate_password("12345678"));
    CHECK(!ls_setup_validate_password("short"));
    CHECK(ls_setup_validate_api_key(""));
    CHECK(ls_setup_validate_api_key("sk-abc12345"));
    CHECK(!ls_setup_validate_api_key("abc12345"));
    CHECK(!ls_setup_validate_api_key("sk-abc 12345"));
    CHECK(ls_setup_validate_server_url(""));
    CHECK(ls_setup_validate_server_url("https://example.com:8443/langstudy"));
    CHECK(!ls_setup_validate_server_url("ftp://example.com"));
    CHECK(!ls_setup_validate_server_url("https://example.com/a b"));
}

static void test_synth(void)
{
    ls_tts_t first;
    ls_tts_t second;
    CHECK(ls_tts_start(&first, "HH AH L 'OW", 1234));
    CHECK(ls_tts_start(&second, "HH AH L 'OW", 1234));
    CHECK(!ls_tts_done(&first));

    int16_t pcm[4096];
    size_t total = 0;
    bool nonzero = false;
    int peak = 0;
    while (!ls_tts_done(&first) && total < 60000) {
        size_t got = ls_tts_render(&first, pcm, 256);
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            if (pcm[i] != 0) nonzero = true;
            int value = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (value > peak) peak = value;
        }
        total += got;
    }
    CHECK(total > 4000);
    CHECK(total < 60000);
    CHECK(nonzero);
    // The resonator normalization must keep low formants from overloading the
    // output. A clipped voice is both distorted and harsh.
    CHECK(peak < 32000);

    // Chunked and single-shot rendering must match for the same seed.
    int16_t one_shot[4096];
    memset(one_shot, 0, sizeof(one_shot));
    size_t got_one = ls_tts_render(&second, one_shot, 4096);
    ls_tts_t third;
    CHECK(ls_tts_start(&third, "HH AH L 'OW", 1234));
    bool matches = true;
    for (size_t i = 0; i < got_one; i += 256) {
        int16_t chunk[256];
        size_t want = got_one - i < 256 ? got_one - i : 256;
        size_t got = ls_tts_render(&third, chunk, want);
        if (got != want || memcmp(chunk, one_shot + i, got * sizeof(int16_t)) != 0) {
            matches = false;
            break;
        }
    }
    CHECK(matches);

    ls_tts_t invalid;
    CHECK(!ls_tts_start(&invalid, "XX", 1));
    CHECK(!ls_tts_start(&invalid, "", 1));
}

static void test_gen_seed_and_history(void)
{
    // A fresh seed and the learner level must reach the prompt verbatim: they
    // are what stops REFRESH from converging on the same "random" sample.
    char prompt[LS_GEN_PROMPT_BASE_MAX + LS_AVOID_WORDS_MAX];
    size_t len = 0;
    CHECK(ls_gen_build_prompt_ex(prompt, sizeof(prompt), &len, LS_EXAM_IELTS,
                                 NULL, false, 0xA1B2C3D4u, 7));
    CHECK(len > 100);
    CHECK(strstr(prompt, "A1B2C3D4") != NULL);
    CHECK(strstr(prompt, "learner level 7 of 10") != NULL);
    CHECK(strstr(prompt, "IELTS band 6-7") != NULL);

    // The SETTINGS level is the starting point; sessions and answered cards
    // raise it, clamped to 1..10. Out-of-range input is clamped because callers
    // pass a u8 from several sources.
    CHECK(ls_gen_level_for_progress(4, 0, 0) == 4);
    CHECK(ls_gen_level_for_progress(4, 39, 0) == 4);
    CHECK(ls_gen_level_for_progress(4, 40, 0) == 5);
    CHECK(ls_gen_level_for_progress(4, 0, 8) == 5);    // 40 points
    CHECK(ls_gen_level_for_progress(4, 0, 24) == 6);   // 120 points
    CHECK(ls_gen_level_for_progress(4, 0, 48) == 7);   // 240 points
    CHECK(ls_gen_level_for_progress(1, 1000, 1000) == 4);
    CHECK(ls_gen_level_for_progress(10, 1000, 1000) == 10);
    CHECK(ls_gen_level_for_progress(0, 0, 0) == 1);
    CHECK(ls_gen_level_for_progress(255, 0, 0) == 10);

    char clamped[LS_GEN_PROMPT_BASE_MAX + LS_AVOID_WORDS_MAX];
    size_t clamped_len = 0;
    CHECK(ls_gen_build_prompt_ex(clamped, sizeof(clamped), &clamped_len,
                                 LS_EXAM_CET4, NULL, false, 1, 99));
    CHECK(strstr(clamped, "learner level 10 of 10") != NULL);
    CHECK(strstr(clamped, "specialist") != NULL);
    size_t low_len = 0;
    CHECK(ls_gen_build_prompt_ex(clamped, sizeof(clamped), &low_len,
                                 LS_EXAM_CET4, NULL, false, 1, 0));
    CHECK(strstr(clamped, "learner level 1 of 10") != NULL);
    CHECK(strstr(clamped, "1000 most common") != NULL);

    // History appends new words, skips duplicates case-insensitively, and
    // drops the oldest entries when the newest pack no longer fits.
    ls_pack_t pack;
    memset(&pack, 0, sizeof(pack));
    pack.word_count = 3;
    strcpy(pack.words[0].text, "Alpha");
    strcpy(pack.words[1].text, "beta");
    strcpy(pack.words[2].text, "gamma");

    char history[64];
    history[0] = '\0';
    ls_gen_append_history(history, sizeof(history), &pack);
    CHECK(strcmp(history, "Alpha beta gamma") == 0);
    // Re-appending the same words (any capitalization) is a no-op.
    strcpy(pack.words[0].text, "alpha");
    ls_gen_append_history(history, sizeof(history), &pack);
    CHECK(strcmp(history, "Alpha beta gamma") == 0);

    // A small cap forces the oldest words out; the newest pack survives whole.
    char small[16];
    small[0] = '\0';
    ls_pack_t first;
    memset(&first, 0, sizeof(first));
    first.word_count = 4;
    strcpy(first.words[0].text, "aa");
    strcpy(first.words[1].text, "bb");
    strcpy(first.words[2].text, "cc");
    strcpy(first.words[3].text, "dd");
    ls_gen_append_history(small, sizeof(small), &first);
    CHECK(strcmp(small, "aa bb cc dd") == 0);

    ls_pack_t second;
    memset(&second, 0, sizeof(second));
    second.word_count = 2;
    strcpy(second.words[0].text, "ee");
    strcpy(second.words[1].text, "ff");
    ls_gen_append_history(small, sizeof(small), &second);
    CHECK(strcmp(small, "bb cc dd ee ff") == 0);
    CHECK(strlen(small) < sizeof(small));

    // A word longer than the whole buffer is skipped rather than truncating
    // another entry.
    char tiny[8];
    tiny[0] = '\0';
    ls_pack_t long_word;
    memset(&long_word, 0, sizeof(long_word));
    long_word.word_count = 1;
    strcpy(long_word.words[0].text, "abcdefghij");
    ls_gen_append_history(tiny, sizeof(tiny), &long_word);
    CHECK(tiny[0] == '\0');
}

static void test_builtin(void)
{
    ls_pack_t pack;
    CHECK(ls_builtin_load(&pack));
    CHECK(pack.lesson_count == 10);
    // The built-in cue pack stays at 100 words even though refreshed packs now
    // request 60; LS_WORDS_MAX is the parser capacity that keeps it valid.
    CHECK(pack.word_count == 100);
    CHECK(pack.words[0].id == 1001);
    CHECK(pack.words[99].id == 1100);

    // Every built-in word must be synthesizable and findable by text.
    for (uint16_t i = 0; i < pack.word_count; i++) {
        const ls_word_t *word = &pack.words[i];
        CHECK(ls_pron_validate(word->pron));
        CHECK(word->translation[0] != '\0');
        CHECK((unsigned char)word->translation[0] >= 0x80); // Chinese, not ASCII
        CHECK(ls_pack_find_text(&pack, word->text) == word);

        ls_tts_t tts;
        CHECK(ls_tts_start(&tts, word->pron, 1));
    }

    // IDs are stable across loads, which is what progress records rely on.
    ls_pack_t again;
    CHECK(ls_builtin_load(&again));
    CHECK(again.words[42].id == pack.words[42].id);

    size_t body_len = 0;
    const char *body = ls_builtin_pack_body(&body_len);
    CHECK(body != NULL);
    CHECK(body_len > 0);
    CHECK(memcmp(body, "LSPACK|2|10|100\n", 16) == 0);
}

static void test_online_voice_helpers(void)
{
    // Edge Read Aloud v2 token. Vectors generated with edge-tts 7.3.1
    // (drm.generate_sec_ms_gec_v2) for fixed timestamps.
    static const struct {
        uint64_t ts;
        const char *expected;
    } tokens[] = {
        {1700000000ULL,
         "2CD23A2CA0B2595A-F3C846C370E05F9CC83ACC5F995C5F5D87A4D0F14CE8936A0F902E64099E38A2"},
        {1730000000ULL,
         "50065ABF67B90F95-49BDAF24D28ECEB38DAEEC84DEAA71E91F92945EA8A47921C58FB4F8F79A4F5E"},
        {1760000000ULL,
         "6D50DDDB91812C33-DD68EFB01898B0750C9797D51A5FF93471DFDAE6888A1039D532CBDCCE54545D"},
        {1794000000ULL,
         "6EA6AF6A0550F520-17E5A544A1EFE811EA5248AFBD9780637964201A6D3A15ADE53A30AB9D066A4F"},
    };
    for (size_t i = 0; i < sizeof(tokens) / sizeof(tokens[0]); i++) {
        char out[LS_EDGE_GEC_TOKEN_LEN + 1];
        CHECK(ls_edge_gec_token(tokens[i].ts, out));
        CHECK(strcmp(out, tokens[i].expected) == 0);
    }
    char too_early[LS_EDGE_GEC_TOKEN_LEN + 1];
    CHECK(!ls_edge_gec_token(1000, too_early));
    CHECK(too_early[0] == '\0');

    // Client frames are masked; the mask XORs the payload.
    const uint8_t mask[4] = {1, 2, 3, 4};
    uint8_t frame[32];
    size_t len = ls_ws_build_frame(LS_WS_OPCODE_TEXT, "hi", 2, mask, frame,
                                   sizeof(frame));
    CHECK(len == 8);
    CHECK(frame[0] == 0x81);
    CHECK(frame[1] == 0x82);
    CHECK(memcmp(frame + 2, mask, 4) == 0);
    CHECK(frame[6] == ('h' ^ 1));
    CHECK(frame[7] == ('i' ^ 2));

    ls_ws_header_t header;
    CHECK(ls_ws_parse_header(frame, len, &header));
    CHECK(header.fin && header.opcode == LS_WS_OPCODE_TEXT && header.masked);
    CHECK(header.payload_len == 2 && header.header_len == 6);

    // 126..65535 uses a two-byte extended length.
    uint8_t big[256];
    memset(big, 0x5A, sizeof(big));
    len = ls_ws_build_frame(LS_WS_OPCODE_BINARY, big, 200, mask, frame,
                            sizeof(frame));
    CHECK(len == 0);  // caller's 32-byte buffer is too small on purpose
    uint8_t big_frame[220];
    len = ls_ws_build_frame(LS_WS_OPCODE_BINARY, big, 200, mask, big_frame,
                            sizeof(big_frame));
    CHECK(len == 208);
    CHECK(big_frame[1] == (0x80 | 126));
    CHECK(ls_ws_parse_header(big_frame, len, &header));
    CHECK(header.payload_len == 200 && header.header_len == 8);

    // Server frames are unmasked and may use the 64-bit length form.
    uint8_t server[14] = {0x82, 0x7E, 0x00, 0x05, 1, 2, 3, 4, 5};
    CHECK(ls_ws_parse_header(server, sizeof(server), &header));
    CHECK(header.opcode == LS_WS_OPCODE_BINARY && !header.masked);
    CHECK(header.payload_len == 5 && header.header_len == 4);
    uint8_t wide[14] = {0x81, 0x7F, 0, 0, 0, 0, 0, 0, 1, 2};
    CHECK(ls_ws_parse_header(wide, sizeof(wide), &header));
    CHECK(header.payload_len == 0x102);

    // 24 kHz MP3 frames to the 16 kHz playback rate: 2 of every 3 samples.
    ls_resampler_t rs;
    CHECK(ls_resampler_init(&rs, 24000, 16000));
    int16_t in[1024];
    int16_t out[2048];
    for (int i = 0; i < 1024; i++) in[i] = (int16_t)i;
    size_t produced = ls_resampler_process(&rs, in, 1024, out,
                                           sizeof(out) / sizeof(out[0]));
    CHECK(produced == 683);
    CHECK(out[0] == 0);
    CHECK(out[produced - 1] == in[1023]);
    for (size_t i = 1; i < produced; i++) CHECK(out[i] >= out[i - 1]);

    // 48 kHz to 16 kHz keeps one of every three samples.
    CHECK(ls_resampler_init(&rs, 48000, 16000));
    int16_t in48[300];
    for (int i = 0; i < 300; i++) in48[i] = (int16_t)(100 + i);
    produced = ls_resampler_process(&rs, in48, 300, out,
                                    sizeof(out) / sizeof(out[0]));
    CHECK(produced == 100);
    CHECK(out[0] == 100 && out[1] == 103 && out[99] == 397);
}

int main(void)
{
    test_builtin();
    test_phoneme();
    test_content();
    test_progress_and_session();
    test_session_rotation();
    test_remap_and_prune();
    test_json_and_gen();
    test_gen_seed_and_history();
    test_sanitize_pack();
    test_autoplay();
    test_setup_form();
    test_synth();
    test_online_voice_helpers();

    if (s_failures) {
        fprintf(stderr, "%d langstudy logic test(s) failed\n", s_failures);
        return 1;
    }
    printf("langstudy logic tests: PASS\n");
    return 0;
}
