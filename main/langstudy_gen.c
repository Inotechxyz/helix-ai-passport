// main/langstudy_gen.c
#include "langstudy_gen.h"

#include "langstudy_content.h"
#include "langstudy_json.h"
#include "langstudy_phoneme.h"

#include <stdio.h>
#include <string.h>

#define SYSTEM_PROMPT \
    "You generate English vocabulary packs. Reply with a JSON object only."

static const char *exam_description(ls_exam_t exam)
{
    switch (exam) {
    case LS_EXAM_CET6: return "CET-6 (Chinese College English Test Band 6)";
    case LS_EXAM_IELTS: return "IELTS academic";
    case LS_EXAM_CET4:
    default: return "CET-4 (Chinese College English Test Band 4)";
    }
}

// Count the space-separated tokens in the avoid list. The builder always uses
// single spaces, so this is exact.
static unsigned count_words(const char *list)
{
    unsigned count = 0;
    bool in_word = false;
    for (const char *p = list; p && *p; p++) {
        if (*p == ' ') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            count++;
        }
    }
    return count;
}

// Shared body of the prompt. The exam name is substituted in twice: once to
// set the vocabulary level and once in the word-selection rule.
//
// The contract is deliberately explicit. DeepSeek tends to answer with a
// prose preamble, a markdown fence, or 50-odd WORD lines when the format is
// only described loosely; every deviation costs a full 60-word retry on this
// device. The size of this text is bounded by PROMPT_CAP in
// langstudy_deepseek.c: base text + worst-case avoid list must stay below it.
static const char s_prompt_format[] =
    "Create a %s English vocabulary pack for Chinese learners.\n"
    "Return exactly one JSON object with one key, \"pack\", and nothing else. "
    "No markdown, code fences, comments, or extra keys.\n"
    "The pack value is one string with \\n escapes between lines, like:\n"
    "{\"pack\":\"LSPACK|2|6|60\\nLESSON|1|Lesson title\\n"
    "WORD|1|1|word|pos|meaning|translation|example|pron\\n...\"}\n"
    "The string must start with LSPACK|2|6|60 and end after the 60th WORD "
    "line. Do not add an END line; the device appends it.\n"
    "Lines, in order:\n"
    "- 6 LESSON lines: LESSON|lesson_id|title, ids 1..6.\n"
    "- 60 WORD lines: WORD|word_id|lesson_id|word|pos|meaning|translation|"
    "example|pron.\n"
    "- 10 words per lesson; word ids 1..60 in order; the device renumbers "
    "words.\n"
    "Rules:\n"
    "- Randomly sample 60 useful %s words at the requested level. Mix "
    "different topics; do not walk the alphabet or reuse an earlier list.\n"
    "- Every WORD line has exactly 9 pipe-separated fields; never use | "
    "inside a field.\n"
    "- English fields are ASCII. word: 1-20 chars. pos: 0-5 chars "
    "(n., v., adj., adv., prep.).\n"
    "- meaning: 1-32 chars (aim for 8-16), a short English definition.\n"
    "- translation: 1-16 common simplified Chinese characters (UTF-8), no "
    "pinyin (aim for 2-6 characters).\n"
    "- example: 0-56 chars (aim for 12-28), one short English sentence using "
    "the word.\n"
    "- pron: required, 1-48 chars (aim for 16-26) of space-separated ARPAbet "
    "phonemes (AA AE AH AO AW "
    "AY B CH D DH EH ER EY F G HH IH IY JH K L M N NG OW OY P R S SH T TH UH "
    "UW V W Y Z ZH); plain ASCII only, never IPA or /slashes/; mark primary "
    "stress with ' before a vowel, for example: HH AH L 'OW. Never leave pron "
    "empty; a WORD line without it is dropped.\n"
    "- All 60 words must be unique (case-insensitive) and short enough for a "
    "240x320 screen.\n"
    "- Uniqueness is enforced by the device: if a word appears twice or "
    "matches the do-not-reuse list, the whole pack is rejected. Check every "
    "word before replying and replace any match.\n"
    "- Write all 60 WORD lines; never stop early. Keep every field compact so "
    "the complete pack fits in one response; a missing line is worse than a "
    "short example.\n";

uint8_t ls_gen_level_for_progress(uint8_t base_level, uint32_t answered_total,
                                  uint32_t session_count)
{
    if (base_level < 1) base_level = 1;
    if (base_level > 10) base_level = 10;

    // Sessions expose new words; answers prove engagement. Weighting sessions
    // keeps a learner who studies regularly but scores poorly from stalling at
    // the starting level forever. The boost tops out at three levels so a
    // starter who keeps going eventually reaches professional vocabulary.
    uint32_t score = answered_total + session_count * 5u;
    uint8_t boost;
    if (score < 40) {
        boost = 0;
    } else if (score < 120) {
        boost = 1;
    } else if (score < 240) {
        boost = 2;
    } else {
        boost = 3;
    }

    unsigned level = (unsigned)base_level + boost;
    return (uint8_t)(level > 10 ? 10 : level);
}

// One short selection hint per level. The prompt asks the model to prefer this
// band, so the wording matters more than exact word counts.
static const char *level_description(uint8_t level)
{
    switch (level) {
    case 1: return "the 1000 most common everyday words";
    case 2: return "the 2000 most common everyday words";
    case 3: return "the 3000 most common everyday words";
    case 4: return "common CET-4 vocabulary";
    case 5: return "strong CET-4 and easy CET-6 vocabulary";
    case 6: return "CET-6 vocabulary";
    case 7: return "academic CET-6 and IELTS band 6-7 vocabulary";
    case 8: return "IELTS band 7-8 and TOEFL academic vocabulary";
    case 9: return "IELTS band 8-9 and GRE-level vocabulary";
    default: return "specialist, rare, and idiomatic vocabulary";
    }
}

bool ls_gen_build_prompt_ex(char *out, size_t cap, size_t *out_len,
                            ls_exam_t exam, const char *avoid_words,
                            bool strict_avoid, uint32_t seed,
                            uint8_t level)
{
    if (!out || cap == 0 || !out_len) return false;
    if (level == 0) level = 1;
    if (level > 10) level = 10;

    const char *exam_text = exam_description(exam);
    int n = snprintf(out, cap, s_prompt_format, exam_text, exam_text);
    if (n < 0 || (size_t)n >= cap) return false;
    size_t used = (size_t)n;

    // A fresh seed on every generation stops the model from settling into the
    // same "random" sample. The level starts at the user's SETTINGS choice and
    // is nudged upward by accumulated sessions and answers; neither value is
    // trusted for correctness (the device still dedupes and enforces the avoid
    // list).
    n = snprintf(out + used, cap - used,
                 "- Pack seed: %08lX (learner level %u of 10; 1 = starter, "
                 "10 = professional). Treat it as a random "
                 "topics/letters/frequency selector so this list differs from "
                 "every earlier one; prefer %s.\n",
                 (unsigned long)seed, (unsigned)level,
                 level_description(level));
    if (n < 0 || (size_t)n >= cap - used) return false;
    used += (size_t)n;

    if (avoid_words && avoid_words[0]) {
        // The model tends to reuse common words. Listing the learner's current
        // words (plus an explicit count) makes refresh after refresh yield
        // different vocabulary; the worker additionally verifies the result.
        // Learned words are listed first by the caller, so they survive if an
        // unusually long pack ever overflows the list cap.
        n = snprintf(out + used, cap - used,
                     "- Do NOT reuse any of the %u words already learned or "
                     "shown in the learner's current pack (case-insensitive). "
                     "Generate a new set of 60 different words, excluding "
                     "plural and tense forms of these words: %s\n",
                     count_words(avoid_words), avoid_words);
        if (n < 0 || (size_t)n >= cap - used) return false;
        used += (size_t)n;

        if (strict_avoid) {
            // A previous reply was rejected for repeating vocabulary. Say so
            // explicitly and push the model toward less common words instead
            // of retrying a similar sample.
            n = snprintf(out + used, cap - used,
                         "- STRICT: your previous reply was rejected because "
                         "it repeated words from that list. Every one of the 60 "
                         "WORDS must be absent from it, including different "
                         "forms. Choose less common words if necessary and "
                         "recheck all 60 lines before replying.\n");
            if (n < 0 || (size_t)n >= cap - used) return false;
            used += (size_t)n;
        }
    }

    *out_len = used;
    return true;
}

bool ls_gen_build_prompt(char *out, size_t cap, size_t *out_len,
                         ls_exam_t exam, const char *avoid_words,
                         bool strict_avoid)
{
    return ls_gen_build_prompt_ex(out, cap, out_len, exam, avoid_words,
                                  strict_avoid, 0, 1);
}

typedef struct {
    char *out;
    size_t cap;
    size_t used;
    bool ok;
} json_writer_t;

static void writer_put(json_writer_t *w, const char *data, size_t len)
{
    if (!w->ok) return;
    if (w->used + len >= w->cap) {
        w->ok = false;
        return;
    }
    memcpy(w->out + w->used, data, len);
    w->used += len;
}

static void writer_puts(json_writer_t *w, const char *text)
{
    writer_put(w, text, strlen(text));
}

// Shared request envelope. The fixed-size builder, the length helper and the
// streaming emitter must agree byte for byte; keeping the pieces in one place
// is what guarantees that Content-Length matches the streamed body.
#define REQ_MODEL_PREFIX "{\"model\":\""
#define REQ_MODEL_SUFFIX "\",\"messages\":[{\"role\":\"system\",\"content\":\""
#define REQ_USER_PREFIX "\"},{\"role\":\"user\",\"content\":\""
#define REQ_SUFFIX \
    "\"}],\"response_format\":{\"type\":\"json_object\"}," \
    "\"temperature\":0.5,\"max_tokens\":8192,\"stream\":true}"

// One escape table feeds both the fixed-size writer and the streaming sink.
typedef void (*escape_put_t)(void *ctx, const char *data, size_t len);

static void escape_emit(const char *text, escape_put_t put, void *ctx)
{
    static const char hex[] = "0123456789abcdef";
    for (const char *p = text; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        const char *escape = NULL;
        char unicode[7];
        switch (c) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default:
            if (c < 0x20) {
                unicode[0] = '\\';
                unicode[1] = 'u';
                unicode[2] = '0';
                unicode[3] = '0';
                unicode[4] = hex[(c >> 4) & 0x0F];
                unicode[5] = hex[c & 0x0F];
                unicode[6] = '\0';
                escape = unicode;
            }
            break;
        }
        if (escape) {
            put(ctx, escape, strlen(escape));
        } else {
            put(ctx, (const char *)&c, 1);
        }
    }
}

static void writer_escape_put(void *ctx, const char *data, size_t len)
{
    writer_put((json_writer_t *)ctx, data, len);
}

static void count_put(void *ctx, const char *data, size_t len)
{
    (void)data;
    *(size_t *)ctx += len;
}

bool ls_gen_build_request(char *out, size_t cap, const char *model,
                          const char *prompt, size_t *out_len)
{
    if (!out || cap == 0 || !model || !prompt || !out_len) return false;

    json_writer_t w = {.out = out, .cap = cap, .used = 0, .ok = true};
    writer_puts(&w, REQ_MODEL_PREFIX);
    escape_emit(model, writer_escape_put, &w);
    writer_puts(&w, REQ_MODEL_SUFFIX);
    escape_emit(SYSTEM_PROMPT, writer_escape_put, &w);
    writer_puts(&w, REQ_USER_PREFIX);
    escape_emit(prompt, writer_escape_put, &w);
    // Streaming keeps bytes flowing while the model generates, so one slow
    // generation cannot trip a blocking read timeout the way stream=false can.
    // Lower temperature keeps the long, rigid line format stable; variety
    // comes from the explicit avoid list rather than sampling.
    writer_puts(&w, REQ_SUFFIX);
    if (!w.ok) return false;
    out[w.used] = '\0';
    *out_len = w.used;
    return true;
}

size_t ls_gen_request_size(const char *model, const char *prompt)
{
    if (!model || !prompt) return 0;
    size_t n = strlen(REQ_MODEL_PREFIX) + strlen(REQ_MODEL_SUFFIX) +
               strlen(REQ_USER_PREFIX) + strlen(REQ_SUFFIX);
    escape_emit(model, count_put, &n);
    escape_emit(SYSTEM_PROMPT, count_put, &n);
    escape_emit(prompt, count_put, &n);
    return n;
}

// Fixed stack buffer that forwards the streamed request to the sink. 256 bytes
// keeps the handshake live set flat while still writing in useful chunks.
typedef struct {
    ls_gen_sink_t sink;
    void *ctx;
    char buf[256];
    size_t used;
    bool ok;
} sink_writer_t;

static void sink_flush(sink_writer_t *w)
{
    if (!w->ok || w->used == 0) return;
    if (!w->sink(w->ctx, w->buf, w->used)) {
        w->ok = false;
        return;
    }
    w->used = 0;
}

static void sink_put(void *ctx, const char *data, size_t len)
{
    sink_writer_t *w = (sink_writer_t *)ctx;
    while (w->ok && len > 0) {
        size_t room = sizeof(w->buf) - w->used;
        size_t take = len < room ? len : room;
        memcpy(w->buf + w->used, data, take);
        w->used += take;
        data += take;
        len -= take;
        if (w->used == sizeof(w->buf)) sink_flush(w);
    }
}

static void sink_puts(sink_writer_t *w, const char *text)
{
    sink_put(w, text, strlen(text));
}

bool ls_gen_emit_request(ls_gen_sink_t sink, void *ctx, const char *model,
                         const char *prompt)
{
    if (!sink || !model || !prompt) return false;
    sink_writer_t w = {.sink = sink, .ctx = ctx, .used = 0, .ok = true};
    sink_puts(&w, REQ_MODEL_PREFIX);
    escape_emit(model, sink_put, &w);
    sink_puts(&w, REQ_MODEL_SUFFIX);
    escape_emit(SYSTEM_PROMPT, sink_put, &w);
    sink_puts(&w, REQ_USER_PREFIX);
    escape_emit(prompt, sink_put, &w);
    sink_puts(&w, REQ_SUFFIX);
    sink_flush(&w);
    return w.ok;
}

// Incremental response reader. The states mirror parse_string() from
// langstudy_json.c, but byte-at-a-time so TLS records can be consumed as they
// arrive and the raw response never needs a buffer of its own.
enum {
    GEN_SCAN = 0,       // outside any string
    GEN_KEY,            // inside a candidate key string
    GEN_KEY_ESC,        // backslash pending inside a key string
    GEN_COLON,          // matched key, skipping whitespace to ':'
    GEN_VALUE_START,    // after ':', skipping whitespace to the quote
    GEN_VALUE,          // inside the matched value string
    GEN_VALUE_ESC,      // backslash pending inside the value
    GEN_VALUE_U,        // reading the four hex digits of \uXXXX
    GEN_VALUE_LOW_BS,   // high surrogate seen, expecting '\'
    GEN_VALUE_LOW_U,    // high surrogate seen, expecting 'u'
    GEN_DONE,
};

static bool reader_put(ls_gen_reader_t *r, const char *data, size_t len)
{
    // Keep one byte for the terminating NUL, matching parse_string().
    if (r->used + len >= r->cap) {
        r->failed = true;
        return false;
    }
    memcpy(r->out + r->used, data, len);
    r->used += len;
    return true;
}

static bool reader_put_byte(ls_gen_reader_t *r, char c)
{
    return reader_put(r, &c, 1);
}

static bool reader_put_codepoint(ls_gen_reader_t *r, uint32_t codepoint)
{
    unsigned char encoded[4];
    size_t encoded_len;
    if (codepoint < 0x80) {
        encoded[0] = (unsigned char)codepoint;
        encoded_len = 1;
    } else if (codepoint < 0x800) {
        encoded[0] = (unsigned char)(0xC0 | (codepoint >> 6));
        encoded[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
        encoded_len = 2;
    } else if (codepoint < 0x10000) {
        encoded[0] = (unsigned char)(0xE0 | (codepoint >> 12));
        encoded[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
        encoded_len = 3;
    } else {
        encoded[0] = (unsigned char)(0xF0 | (codepoint >> 18));
        encoded[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        encoded[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
        encoded_len = 4;
    }
    return reader_put(r, (const char *)encoded, encoded_len);
}

static bool reader_fail(ls_gen_reader_t *r)
{
    r->failed = true;
    return false;
}

void ls_gen_reader_init(ls_gen_reader_t *reader, char *out, size_t cap)
{
    if (!reader) return;
    memset(reader, 0, sizeof(*reader));
    reader->out = out;
    reader->cap = cap;
    reader->state = GEN_SCAN;
    if (!out || cap == 0) reader->failed = true;
}

bool ls_gen_reader_feed(ls_gen_reader_t *reader, const char *data, size_t len)
{
    if (!reader || reader->failed) return false;
    if (!data && len) return reader_fail(reader);
    if (reader->state == GEN_DONE) return true;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

    again:
        switch (reader->state) {
        case GEN_SCAN:
            if (c == '"') {
                reader->state = GEN_KEY;
                reader->key_len = 0;
                // Bit 0 tracks "content", bit 1 tracks "pack".
                reader->key_state = 3;
            }
            break;

        case GEN_KEY:
            if (c == '"') {
                uint8_t key = 0;
                if ((reader->key_state & 1) && reader->key_len == 7) key = 1;
                else if ((reader->key_state & 2) && reader->key_len == 4) key = 2;
                if (key) {
                    reader->matched_key = key;
                    reader->state = GEN_COLON;
                } else {
                    reader->state = GEN_SCAN;
                }
            } else if (c == '\\') {
                // Neither target key contains an escape, so stop matching but
                // keep scanning for the real closing quote.
                reader->key_state = 0;
                reader->state = GEN_KEY_ESC;
            } else if (c < 0x20) {
                return reader_fail(reader);
            } else {
                size_t pos = reader->key_len;
                if (pos >= 7) {
                    reader->key_state = 0;
                } else {
                    static const char content[] = "content";
                    static const char pack[] = "pack";
                    if (content[pos] != (char)c) {
                        reader->key_state &= (uint8_t)~1;
                    }
                    if (pos >= 4 || pack[pos] != (char)c) {
                        reader->key_state &= (uint8_t)~2;
                    }
                }
                reader->key_len++;
            }
            break;

        case GEN_KEY_ESC:
            // The escaped byte cannot end the string; \uXXXX hex digits are
            // ordinary characters for termination purposes.
            reader->state = GEN_KEY;
            break;

        case GEN_COLON:
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            if (c == ':') {
                reader->state = GEN_VALUE_START;
                break;
            }
            reader->matched_key = 0;
            reader->state = GEN_SCAN;
            goto again;

        case GEN_VALUE_START:
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            if (c == '"') {
                reader->used = 0;
                reader->codepoint = 0;
                reader->pending_high = 0;
                reader->hex_count = 0;
                reader->state = GEN_VALUE;
            } else {
                reader->matched_key = 0;
                reader->state = GEN_SCAN;
                goto again;
            }
            break;

        case GEN_VALUE:
            if (c == '"') {
                reader->value_len = reader->used;
                reader->state = GEN_DONE;
            } else if (c < 0x20) {
                return reader_fail(reader);
            } else if (c == '\\') {
                reader->state = GEN_VALUE_ESC;
            } else if (!reader_put_byte(reader, (char)c)) {
                return false;
            }
            break;

        case GEN_VALUE_ESC: {
            int decoded = 0;
            switch (c) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u':
                reader->codepoint = 0;
                reader->hex_count = 0;
                reader->state = GEN_VALUE_U;
                break;
            default:
                return reader_fail(reader);
            }
            if (c == 'u') break;
            if (!reader_put_byte(reader, (char)decoded)) return false;
            reader->state = GEN_VALUE;
            break;
        }

        case GEN_VALUE_U: {
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return reader_fail(reader);

            reader->codepoint = (reader->codepoint << 4) | (uint32_t)digit;
            if (++reader->hex_count < 4) break;

            uint32_t value = reader->codepoint;
            if (reader->pending_high) {
                if (value < 0xDC00 || value > 0xDFFF) return reader_fail(reader);
                uint32_t codepoint =
                    0x10000u + (((uint32_t)reader->pending_high - 0xD800u) << 10) +
                    (value - 0xDC00u);
                reader->pending_high = 0;
                reader->state = GEN_VALUE;
                if (!reader_put_codepoint(reader, codepoint)) return false;
            } else if (value >= 0xD800 && value <= 0xDBFF) {
                reader->pending_high = (uint16_t)value;
                reader->state = GEN_VALUE_LOW_BS;
            } else if (value >= 0xDC00 && value <= 0xDFFF) {
                return reader_fail(reader);
            } else {
                reader->state = GEN_VALUE;
                if (!reader_put_codepoint(reader, value)) return false;
            }
            break;
        }

        case GEN_VALUE_LOW_BS:
            if (c != '\\') return reader_fail(reader);
            reader->state = GEN_VALUE_LOW_U;
            break;

        case GEN_VALUE_LOW_U:
            if (c != 'u') return reader_fail(reader);
            reader->codepoint = 0;
            reader->hex_count = 0;
            reader->state = GEN_VALUE_U;
            break;

        case GEN_DONE:
            break;
        }
    }
    return !reader->failed;
}

bool ls_gen_reader_finish(ls_gen_reader_t *reader, size_t *pack_len)
{
    if (!reader || !pack_len) return false;
    *pack_len = 0;
    if (reader->failed || reader->state != GEN_DONE) return false;

    char *text = reader->out;
    size_t len = reader->value_len;
    text[len] = '\0';

    if (reader->matched_key == 1 && !(len >= 6 && memcmp(text, "LSPACK", 6) == 0)) {
        // Content is often itself a {"pack":"..."} JSON object. Decode it in
        // place; the output cursor trails the read cursor, so this is safe.
        size_t extracted = 0;
        if (!ls_json_extract_string(text, len, "pack", text, reader->cap,
                                    &extracted)) {
            return false;
        }
        len = extracted;
        text[len] = '\0';
    } else if (reader->matched_key != 1 && reader->matched_key != 2) {
        return false;
    }

    if (len < 6 || memcmp(text, "LSPACK", 6) != 0) return false;
    *pack_len = len;
    return ls_pack_finalize(text, pack_len, reader->cap);
}

bool ls_gen_finalize_pack(char *text, size_t *len, size_t cap)
{
    if (!text || !len || cap == 0) return false;
    size_t used = *len;
    if (used == 0 || used >= cap) return false;
    text[used] = '\0';

    if (!(used >= 6 && memcmp(text, "LSPACK", 6) == 0)) {
        // The streamed content may be the {"pack":"..."} wrapper itself.
        // Decode it in place: the output cursor trails the read cursor.
        size_t extracted = 0;
        if (!ls_json_extract_string(text, used, "pack", text, cap, &extracted)) {
            return false;
        }
        used = extracted;
        text[used] = '\0';
    }
    if (used < 6 || memcmp(text, "LSPACK", 6) != 0) return false;

    *len = used;
    return ls_pack_finalize(text, len, cap);
}

// The model does not always follow the line contract. IELTS replies in
// particular tend to carry IPA in `pron` ("/ˈtenənt/"), use smart quotes in
// `example`, or overshoot a field limit. Rejecting the whole 60-word reply for
// one bad field costs another full generation, so the sanitizer rewrites each
// line instead: fields are trimmed to the format limits, non-ASCII punctuation
// is transliterated to ASCII, a phonetic that is not valid ARPAbet is cleared
// (the word stays usable, it just has no spoken pronunciation), and words with
// unusable required fields are dropped. The header counts and CRC are then
// rebuilt around what survived.

#define SAN_LINE_MAX 320
#define SAN_FIELD_MAX 12

typedef struct {
    const char *ptr;
    size_t len;
} san_field_t;

static size_t san_split(const char *line, size_t len, char sep,
                        san_field_t *fields, size_t max_fields)
{
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || line[i] == sep) {
            if (count < max_fields) {
                fields[count].ptr = line + start;
                fields[count].len = i - start;
                count++;
            }
            start = i + 1;
        }
    }
    return count;
}

static bool san_u32(const san_field_t *field, uint32_t *out)
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

// Length of a valid UTF-8 sequence at `s`, or 0 when the bytes are invalid.
static size_t san_utf8_len(const char *s, size_t len)
{
    unsigned char c = (unsigned char)s[0];
    size_t step;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        step = 2;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        step = 3;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        step = 4;
        cp = c & 0x07u;
    } else {
        return 0;
    }
    if (step > len) return 0;
    for (size_t i = 1; i < step; i++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((step == 2 && cp < 0x80) || (step == 3 && cp < 0x800) ||
        (step == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
        return 0;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp == '|' || cp < 0x20) return 0;
    return step;
}

// Copy a printable-ASCII field. Common Unicode punctuation is mapped to its
// ASCII counterpart; other non-ASCII code points are dropped. Runs of spaces
// collapse so a removed glyph does not leave a hole in the sentence.
static size_t san_ascii(char *dst, size_t cap, const char *src, size_t len,
                        size_t max_len)
{
    if (!dst || cap == 0 || max_len == 0) return 0;
    size_t limit = max_len < cap - 1 ? max_len : cap - 1;
    size_t used = 0;
    bool have_char = false;
    bool space_pending = false;

    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)src[i];
        char out = '\0';
        size_t step = 1;

        if (c == ' ' || c == '\t') {
            space_pending = have_char;
            i++;
            continue;
        }
        if (c >= 0x20 && c <= 0x7E && c != '|') {
            out = (char)c;
        } else if (c == 0xE2 && i + 2 < len &&
                   (unsigned char)src[i + 1] == 0x80) {
            switch ((unsigned char)src[i + 2]) {
            case 0x98:
            case 0x99: out = '\''; break;  // curly single quotes
            case 0x9C:
            case 0x9D: out = '"'; break;   // curly double quotes
            case 0x93:
            case 0x94: out = '-'; break;   // en/em dash
            case 0xA6: out = '.'; break;   // ellipsis
            default: break;
            }
            step = 3;
        } else if (c == 0xC2 && i + 1 < len &&
                   (unsigned char)src[i + 1] == 0xA0) {
            out = ' ';  // non-breaking space
            step = 2;
        } else {
            // Drop one complete UTF-8 sequence (or a stray byte).
            size_t skip = san_utf8_len(src + i, len - i);
            if (skip == 0) {
                if ((c & 0xE0) == 0xC0) skip = 2;
                else if ((c & 0xF0) == 0xE0) skip = 3;
                else if ((c & 0xF8) == 0xF0) skip = 4;
                else skip = 1;
            }
            if (i + skip > len) skip = len - i;
            i += skip;
            continue;
        }

        if (out == ' ') {
            space_pending = have_char;
            i += step;
            continue;
        }
        if (space_pending && used < limit) {
            dst[used++] = ' ';
            space_pending = false;
        }
        if (used < limit) {
            dst[used++] = out;
            have_char = true;
        }
        i += step;
    }
    dst[used] = '\0';
    return used;
}

static bool san_has_non_ascii(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)text[i] >= 0x80) return true;
    }
    return false;
}

// Copy a UTF-8 field (the Chinese translation). Valid sequences are kept whole
// so the display never receives a split character; invalid bytes are dropped.
static size_t san_utf8(char *dst, size_t cap, const char *src, size_t len,
                       size_t max_len)
{
    if (!dst || cap == 0 || max_len == 0) return 0;
    size_t limit = max_len < cap - 1 ? max_len : cap - 1;
    size_t used = 0;

    for (size_t i = 0; i < len && used < limit;) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ' || c == '\t') {
            if (used > 0 && dst[used - 1] != ' ' && used < limit) {
                dst[used++] = ' ';
            }
            i++;
            continue;
        }
        if (c >= 0x20 && c <= 0x7E && c != '|') {
            dst[used++] = (char)c;
            i++;
            continue;
        }
        size_t step = san_utf8_len(src + i, len - i);
        if (step == 0) {
            i++;
            continue;
        }
        if (used + step > limit) break;  // never split a code point
        memcpy(dst + used, src + i, step);
        used += step;
        i += step;
    }
    while (used > 0 && dst[used - 1] == ' ') used--;
    dst[used] = '\0';
    return used;
}

static bool san_ci_equal(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

void ls_gen_append_history(char *history, size_t cap, const ls_pack_t *pack)
{
    if (!history || cap < 2 || !pack) return;
    // Bounded length: a truncated snapshot must not be read past the buffer.
    size_t used = 0;
    while (used < cap - 1 && history[used]) used++;
    history[used] = '\0';

    size_t words = pack->word_count;
    if (words > LS_WORDS_MAX) words = LS_WORDS_MAX;

    for (size_t i = 0; i < words; i++) {
        const char *word = pack->words[i].text;
        size_t len = strlen(word);
        if (len == 0 || len >= cap - 1) continue;
        if (memchr(word, ' ', len) || memchr(word, '|', len)) continue;

        // The history is a set: a word already listed is not appended again.
        bool seen = false;
        for (size_t pos = 0; pos < used;) {
            size_t end = pos;
            while (end < used && history[end] != ' ') end++;
            if (san_ci_equal(history + pos, end - pos, word)) {
                seen = true;
                break;
            }
            pos = end + 1;
        }
        if (seen) continue;

        // Drop the oldest words until the newest one fits with its separator
        // and the terminating NUL.
        size_t need = len + (used ? 1 : 0);
        while (used + need >= cap) {
            char *space = memchr(history, ' ', used);
            if (!space) {
                used = 0;
                break;
            }
            size_t drop = (size_t)(space - history) + 1;
            memmove(history, history + drop, used - drop);
            used -= drop;
            need = len + (used ? 1 : 0);
        }
        if (used + need >= cap) continue;

        if (used) history[used++] = ' ';
        memcpy(history + used, word, len);
        used += len;
        history[used] = '\0';
    }
}

static bool san_body_has_word(const char *body, size_t len, const char *word)
{
    size_t pos = 0;
    while (pos < len) {
        const char *line = body + pos;
        const char *nl = memchr(line, '\n', len - pos);
        if (!nl) break;
        size_t line_len = (size_t)(nl - line);
        if (line_len >= 5 && memcmp(line, "WORD|", 5) == 0) {
            san_field_t fields[SAN_FIELD_MAX];
            size_t count = san_split(line, line_len, '|', fields,
                                     SAN_FIELD_MAX);
            if (count > 3 && san_ci_equal(fields[3].ptr, fields[3].len, word)) {
                return true;
            }
        }
        pos += line_len + 1;
    }
    return false;
}

static bool san_body_has_id(const char *body, size_t len, uint32_t id)
{
    size_t pos = 0;
    while (pos < len) {
        const char *line = body + pos;
        const char *nl = memchr(line, '\n', len - pos);
        if (!nl) break;
        size_t line_len = (size_t)(nl - line);
        if (line_len >= 5 && memcmp(line, "WORD|", 5) == 0) {
            san_field_t fields[SAN_FIELD_MAX];
            size_t count = san_split(line, line_len, '|', fields,
                                     SAN_FIELD_MAX);
            uint32_t other = 0;
            if (count > 1 && san_u32(&fields[1], &other) && other == id) {
                return true;
            }
        }
        pos += line_len + 1;
    }
    return false;
}

// Lesson ids are collected first so a WORD line whose lesson_id is missing or
// unknown can still be attached to the first valid lesson.
static size_t san_collect_lesson_ids(const char *text, size_t used,
                                     size_t body_start, uint32_t *ids,
                                     size_t cap)
{
    size_t count = 0;
    size_t pos = body_start;
    while (pos < used) {
        const char *line = text + pos;
        const char *nl = memchr(line, '\n', used - pos);
        if (!nl) break;
        size_t line_len = (size_t)(nl - line);
        if (line_len >= 7 && memcmp(line, "LESSON|", 7) == 0) {
            san_field_t fields[SAN_FIELD_MAX];
            size_t n = san_split(line, line_len, '|', fields, SAN_FIELD_MAX);
            uint32_t id = 0;
            if (n >= 2 && san_u32(&fields[1], &id) && id != 0 &&
                count < cap) {
                bool duplicate = false;
                for (size_t i = 0; i < count; i++) {
                    if (ids[i] == id) duplicate = true;
                }
                if (!duplicate) ids[count++] = id;
            }
        } else if (!(line_len >= 5 && memcmp(line, "WORD|", 5) == 0)) {
            break;
        }
        pos += line_len + 1;
    }
    return count;
}

static bool san_lesson_line(char *line, size_t cap, const san_field_t *fields,
                            size_t count, uint32_t id)
{
    char raw[SAN_LINE_MAX];
    size_t raw_len = 0;
    for (size_t i = 2; i < count; i++) {
        size_t need = fields[i].len + (raw_len ? 1 : 0);
        if (raw_len + need >= sizeof(raw)) break;
        if (raw_len) raw[raw_len++] = ' ';
        memcpy(raw + raw_len, fields[i].ptr, fields[i].len);
        raw_len += fields[i].len;
    }
    char title[LS_LESSON_TITLE_MAX + 1];
    san_ascii(title, sizeof(title), raw, raw_len, LS_LESSON_TITLE_MAX);
    if (title[0] == '\0') {
        snprintf(title, sizeof(title), "Lesson %lu", (unsigned long)id);
    }
    int written = snprintf(line, cap, "LESSON|%lu|%s\n", (unsigned long)id,
                           title);
    return written > 0 && (size_t)written < cap;
}

static bool san_word_line(char *line, size_t cap, const san_field_t *fields,
                          size_t count, const uint32_t *lesson_ids,
                          size_t lesson_count, const char *body,
                          size_t body_len, uint32_t fallback_id)
{
    if (count < 9) return false;

    char word[LS_WORD_TEXT_MAX + 1];
    char pos[LS_POS_MAX + 1];
    char meaning[LS_MEANING_MAX + 1];
    char translation[LS_TRANSLATION_MAX + 1];
    char example[LS_EXAMPLE_MAX + 1];
    char pron[LS_PRON_MAX + 1];

    san_ascii(word, sizeof(word), fields[3].ptr, fields[3].len,
              LS_WORD_TEXT_MAX);
    if (word[0] == '\0') return false;
    if (san_body_has_word(body, body_len, word)) return false;

    san_ascii(meaning, sizeof(meaning), fields[5].ptr, fields[5].len,
              LS_MEANING_MAX);
    if (meaning[0] == '\0') return false;

    size_t translation_len =
        san_utf8(translation, sizeof(translation), fields[6].ptr,
                 fields[6].len, LS_TRANSLATION_MAX);
    if (translation_len == 0 || !san_has_non_ascii(translation,
                                                   translation_len)) {
        return false;
    }

    san_ascii(pos, sizeof(pos), fields[4].ptr, fields[4].len, LS_POS_MAX);
    san_ascii(example, sizeof(example), fields[7].ptr, fields[7].len,
              LS_EXAMPLE_MAX);
    san_ascii(pron, sizeof(pron), fields[8].ptr, fields[8].len, LS_PRON_MAX);
    if (pron[0] == '\0' || !ls_pron_validate(pron)) {
        // The model frequently ignores the "plain ARPAbet" instruction and
        // answers with IPA ("/ˈpredʒədɪs/"), or leaves the field empty.
        // san_ascii() has just dropped the non-ASCII symbols, so convert from
        // the untouched field before giving up.
        char ipa[LS_PRON_MAX * 2 + 1];
        size_t ipa_len = fields[8].len;
        if (ipa_len > sizeof(ipa) - 1) ipa_len = sizeof(ipa) - 1;
        memcpy(ipa, fields[8].ptr, ipa_len);
        ipa[ipa_len] = '\0';
        if (ls_ipa_to_arpabet(ipa, pron, sizeof(pron)) == 0 ||
            pron[0] == '\0' || !ls_pron_validate(pron)) {
            // A card with no transcription cannot show IPA or speak the word
            // (Edge TTS and the phoneme synthesizer both need it), so drop the
            // line instead of surfacing a silent word. Header counts are
            // recomputed, and a reply that drops below the minimum usable
            // vocabulary is rejected and retried.
            return false;
        }
    }

    uint32_t id = 0;
    if (!san_u32(&fields[1], &id) || id == 0 ||
        san_body_has_id(body, body_len, id)) {
        id = fallback_id;
    }

    uint32_t lesson_id = 0;
    bool lesson_known = false;
    if (san_u32(&fields[2], &lesson_id) && lesson_id != 0) {
        for (size_t i = 0; i < lesson_count; i++) {
            if (lesson_ids[i] == lesson_id) {
                lesson_known = true;
                break;
            }
        }
    }
    if (!lesson_known) lesson_id = lesson_ids[0];

    int written = snprintf(line, cap, "WORD|%lu|%lu|%s|%s|%s|%s|%s|%s\n",
                           (unsigned long)id, (unsigned long)lesson_id,
                           word, pos, meaning, translation, example, pron);
    return written > 0 && (size_t)written < cap;
}

// Copy a repaired line to the output cursor. Most repairs shrink a line, but
// converting an IPA `pron` to ARPAbet can grow it, so when the write cursor
// would overtake the unread source, shift that source tail right first. Returns
// false when the buffer has no room and the caller should stop accepting lines.
static bool san_place_line(char *text, size_t cap, size_t *used, size_t *out,
                           size_t pos, size_t line_len, const char *line,
                           size_t written, size_t *next_pos)
{
    size_t src_end = pos + line_len + 1;  // the source line plus its newline
    size_t next = src_end;
    if (*out + written > src_end) {
        size_t grow = *out + written - src_end;
        if (*used + grow >= cap) return false;
        memmove(text + src_end + grow, text + src_end, *used - src_end);
        *used += grow;
        next = src_end + grow;
    }
    memcpy(text + *out, line, written);
    *out += written;
    *next_pos = next;
    return true;
}

bool ls_gen_sanitize_pack(char *text, size_t *len, size_t cap,
                          uint16_t min_words)
{
    if (!text || !len || cap < 2) return false;
    size_t used = *len;
    if (used == 0 || used >= cap) return false;
    text[used] = '\0';

    // The model occasionally prefixes the pack with prose. Drop everything
    // before the first header so a stray sentence does not invalidate it.
    char *pack_start = strstr(text, "LSPACK|");
    if (!pack_start) return false;
    if (pack_start != text) {
        used -= (size_t)(pack_start - text);
        memmove(text, pack_start, used);
        text[used] = '\0';
    }
    if (used < 9 || memcmp(text, "LSPACK|2|", 9) != 0) return false;

    char *header_nl = memchr(text, '\n', used);
    if (!header_nl) return false;
    size_t header_len = (size_t)(header_nl - text);
    if (header_len == 0 || header_len >= 40) return false;
    size_t body_start = header_len + 1;

    uint32_t lesson_ids[LS_LESSONS_MAX];
    size_t lesson_total = san_collect_lesson_ids(text, used, body_start,
                                                 lesson_ids, LS_LESSONS_MAX);
    if (lesson_total == 0) return false;

    // Rewrite complete lines into the same buffer. Most repairs shorten a line,
    // but IPA->ARPAbet conversion can make a `pron` field longer; san_place_line
    // shifts the unread tail right when needed, so the write cursor may briefly
    // pass the read cursor. A truncated final line has no newline and is
    // dropped; END or prose ends the accepted prefix.
    size_t pos = body_start;
    size_t out = body_start;
    size_t lesson_count = 0;
    size_t word_count = 0;
    uint32_t written_lesson_ids[LS_LESSONS_MAX];
    char line[SAN_LINE_MAX];

    while (pos < used) {
        const char *src = text + pos;
        const char *nl = memchr(src, '\n', used - pos);
        if (!nl) break;
        size_t line_len = (size_t)(nl - src);
        if (line_len == 0) break;
        size_t next_pos = pos + line_len + 1;

        if (line_len >= 7 && memcmp(src, "LESSON|", 7) == 0) {
            san_field_t fields[SAN_FIELD_MAX];
            size_t n = san_split(src, line_len, '|', fields, SAN_FIELD_MAX);
            uint32_t id = 0;
            bool known = false;
            if (n >= 2 && san_u32(&fields[1], &id) && id != 0) {
                for (size_t i = 0; i < lesson_total; i++) {
                    if (lesson_ids[i] == id) known = true;
                }
                for (size_t i = 0; i < lesson_count; i++) {
                    if (written_lesson_ids[i] == id) known = false;
                }
            }
            if (known &&
                san_lesson_line(line, sizeof(line), fields, n, id)) {
                size_t written = strlen(line);
                if (!san_place_line(text, cap, &used, &out, pos, line_len,
                                    line, written, &next_pos)) {
                    break;
                }
                written_lesson_ids[lesson_count++] = id;
            }
        } else if (line_len >= 5 && memcmp(src, "WORD|", 5) == 0) {
            // Online packs are parsed into an LS_WORD_TARGET-sized staging
            // model, so trim an over-eager reply here instead of letting the
            // parser reject the whole generation for its count.
            if (word_count >= LS_WORD_TARGET) break;
            san_field_t fields[SAN_FIELD_MAX];
            size_t n = san_split(src, line_len, '|', fields, SAN_FIELD_MAX);
            uint32_t fallback_id = 3000000u + (uint32_t)word_count;
            if (san_word_line(line, sizeof(line), fields, n, lesson_ids,
                              lesson_total, text + body_start,
                              out - body_start, fallback_id)) {
                size_t written = strlen(line);
                if (!san_place_line(text, cap, &used, &out, pos, line_len,
                                    line, written, &next_pos)) {
                    break;
                }
                word_count++;
            }
        } else {
            // END or anything else ends the pack body.
            break;
        }
        pos = next_pos;
    }

    if (lesson_count == 0 || word_count < min_words) return false;

    // Rewrite the header so the declared counts match the lines actually
    // present; the parser requires an exact match.
    char header[40];
    int written = snprintf(header, sizeof(header), "LSPACK|%d|%u|%u\n",
                           LS_FORMAT_VERSION, (unsigned)lesson_count,
                           (unsigned)word_count);
    if (written < 0 || (size_t)written >= sizeof(header)) return false;
    size_t body_len = out - body_start;
    if ((size_t)written + body_len >= cap) return false;
    memmove(text + written, text + body_start, body_len);
    memcpy(text, header, (size_t)written);
    size_t new_len = (size_t)written + body_len;
    text[new_len] = '\0';
    *len = new_len;
    return ls_pack_finalize(text, len, cap);
}

bool ls_gen_extract_pack(const char *response, size_t len,
                         char *pack_out, size_t pack_cap, size_t *pack_len)
{
    if (!response || len == 0 || !pack_out || pack_cap == 0 || !pack_len) {
        return false;
    }
    *pack_len = 0;

    size_t content_len = 0;
    if (ls_json_extract_string(response, len, "content", pack_out, pack_cap,
                               &content_len)) {
        if (content_len >= 6 && memcmp(pack_out, "LSPACK", 6) == 0) {
            *pack_len = content_len;
        } else {
            // Decode {"pack":"..."} in place: the output cursor always trails
            // the JSON read cursor, so no second 20 KiB buffer is needed.
            size_t extracted = 0;
            if (ls_json_extract_string(pack_out, content_len, "pack", pack_out,
                                       pack_cap, &extracted)) {
                *pack_len = extracted;
            }
        }
    }

    if (*pack_len == 0) {
        size_t extracted = 0;
        if (ls_json_extract_string(response, len, "pack", pack_out, pack_cap,
                                   &extracted)) {
            *pack_len = extracted;
        }
    }

    if (*pack_len < 6 || memcmp(pack_out, "LSPACK", 6) != 0) return false;
    return ls_pack_finalize(pack_out, pack_len, pack_cap);
}
