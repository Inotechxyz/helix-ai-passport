// main/langstudy_phoneme.c
#include "langstudy_phoneme.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    const char *name;
    ls_phoneme_t value;
} phoneme_name_t;

// Order matches ls_phoneme_t. Lookup is a short linear scan: pronunciations are
// only tokenized when a word is spoken or a pack is validated, never per frame.
static const phoneme_name_t s_names[] = {
    {"AA", LS_PH_AA}, {"AE", LS_PH_AE}, {"AH", LS_PH_AH}, {"AO", LS_PH_AO},
    {"AW", LS_PH_AW}, {"AY", LS_PH_AY}, {"EH", LS_PH_EH}, {"ER", LS_PH_ER},
    {"EY", LS_PH_EY}, {"IH", LS_PH_IH}, {"IY", LS_PH_IY}, {"OW", LS_PH_OW},
    {"OY", LS_PH_OY}, {"UH", LS_PH_UH}, {"UW", LS_PH_UW},
    {"B", LS_PH_B},   {"CH", LS_PH_CH}, {"D", LS_PH_D},   {"DH", LS_PH_DH},
    {"F", LS_PH_F},   {"G", LS_PH_G},   {"HH", LS_PH_HH}, {"JH", LS_PH_JH},
    {"K", LS_PH_K},   {"L", LS_PH_L},   {"M", LS_PH_M},   {"N", LS_PH_N},
    {"NG", LS_PH_NG}, {"P", LS_PH_P},   {"R", LS_PH_R},   {"S", LS_PH_S},
    {"SH", LS_PH_SH}, {"T", LS_PH_T},   {"TH", LS_PH_TH}, {"V", LS_PH_V},
    {"W", LS_PH_W},   {"Y", LS_PH_Y},   {"Z", LS_PH_Z},   {"ZH", LS_PH_ZH},
};

#define PHONEME_NAME_COUNT (sizeof(s_names) / sizeof(s_names[0]))

bool ls_phoneme_parse(const char *token, size_t len,
                      ls_phoneme_t *out, uint8_t *stress)
{
    if (!token || !out || len == 0) return false;

    uint8_t value_stress = 0;
    size_t begin = 0;
    size_t end = len;

    if (token[begin] == '\'' || token[begin] == ',') {
        value_stress = token[begin] == '\'' ? 1 : 2;
        begin++;
    }
    if (end > begin && token[end - 1] >= '0' && token[end - 1] <= '2') {
        value_stress = (uint8_t)(token[end - 1] - '0');
        end--;
    }
    if (end <= begin || end - begin > 2) return false;

    char upper[3] = {0};
    for (size_t i = 0; i < end - begin; i++) {
        unsigned char c = (unsigned char)token[begin + i];
        if (c < 'A' || c > 'Z') c = (unsigned char)toupper(c);
        if (c < 'A' || c > 'Z') return false;
        upper[i] = (char)c;
    }

    for (size_t i = 0; i < PHONEME_NAME_COUNT; i++) {
        if (strcmp(upper, s_names[i].name) == 0) {
            *out = s_names[i].value;
            if (stress) *stress = value_stress;
            return true;
        }
    }
    return false;
}

size_t ls_pron_tokenize(const char *pron, ls_phoneme_t *phonemes,
                        uint8_t *stress, size_t cap)
{
    if (!pron || !phonemes || cap == 0) return 0;

    size_t count = 0;
    const char *p = pron;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (count >= cap) return 0;

        const char *start = p;
        while (*p && *p != ' ') p++;
        ls_phoneme_t value;
        uint8_t token_stress = 0;
        if (!ls_phoneme_parse(start, (size_t)(p - start), &value, &token_stress)) {
            return 0;
        }
        phonemes[count] = value;
        if (stress) stress[count] = token_stress;
        count++;
    }
    return count;
}

bool ls_pron_validate(const char *pron)
{
    if (!pron) return false;
    size_t len = strlen(pron);
    if (len > LS_PRON_MAX) return false;
    if (len == 0) return true;

    ls_phoneme_t phonemes[LS_PRON_TOKENS_MAX];
    return ls_pron_tokenize(pron, phonemes, NULL, LS_PRON_TOKENS_MAX) > 0;
}

const char *ls_phoneme_name(ls_phoneme_t phoneme)
{
    if ((unsigned)phoneme >= LS_PHONEME_COUNT) return "?";
    return s_names[(unsigned)phoneme].name;
}

// ---------------------------------------------------------------------------
// IPA -> ARPAbet fallback
// ---------------------------------------------------------------------------
//
// DeepSeek answers IELTS-style requests with IPA despite the prompt asking for
// space-separated ARPAbet (for example "/ˈpredʒədɪs/"). san_ascii() drops those
// code points as it copies the field, so without a converter every such word
// loses its pronunciation. The table covers the symbols that occur in English
// dictionaries plus the obvious near-neighbours; unknown marks are ignored
// rather than failing the whole transcription.

#define IPA_MAX_CODEPOINTS 96

static size_t ipa_decode(const char *text, size_t len, uint32_t *out,
                         size_t cap)
{
    size_t count = 0;
    for (size_t i = 0; i < len && count < cap;) {
        unsigned char c = (unsigned char)text[i];
        uint32_t cp;
        size_t step;

        if (c < 0x80) {
            cp = c;
            step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            step = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            step = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            step = 4;
        } else {
            i++;
            continue;
        }
        if (i + step > len) break;

        bool valid = true;
        for (size_t k = 1; k < step; k++) {
            unsigned char cc = (unsigned char)text[i + k];
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!valid) {
            i++;
            continue;
        }
        out[count++] = cp;
        i += step;
    }
    return count;
}

static bool ipa_is_vowel_cp(uint32_t cp)
{
    switch (cp) {
    case 'a': case 'e': case 'i': case 'o': case 'u':
    case 0x00E6:  // æ
    case 0x00F8:  // ø
    case 0x0153:  // œ
    case 0x0250:  // ɐ
    case 0x0251:  // ɑ
    case 0x0252:  // ɒ
    case 0x0254:  // ɔ
    case 0x0259:  // ə
    case 0x025A:  // ɚ
    case 0x025B:  // ɛ
    case 0x025C:  // ɜ
    case 0x025D:  // ɝ
    case 0x025E:  // ɞ
    case 0x0264:  // ɤ
    case 0x0268:  // ɨ
    case 0x026A:  // ɪ
    case 0x026F:  // ɯ
    case 0x0289:  // ʉ
    case 0x028A:  // ʊ
    case 0x028C:  // ʌ
        return true;
    default:
        return false;
    }
}

static const char *ipa_consonant(uint32_t cp)
{
    switch (cp) {
    case 'p': return "P";
    case 'b': return "B";
    case 't': return "T";
    case 'd': return "D";
    case 'k': case 'q': case 'c': return "K";
    case 'g': case 0x0261: return "G";  // ɡ
    case 'f': return "F";
    case 'v': return "V";
    case 0x03B8: return "TH";           // θ
    case 0x00F0: return "DH";           // ð
    case 's': return "S";
    case 'z': return "Z";
    case 0x0283: return "SH";           // ʃ
    case 0x0292: return "ZH";           // ʒ
    case 'h': case 0x0266: case 0x029C: case 0x0127:
        return "HH";                    // h ɦ ʜ ħ
    case 'm': case 0x0271: return "M";  // m ɱ
    case 'n': case 0x0272: case 0x0273: return "N";  // n ɲ ɳ
    case 0x014B: return "NG";           // ŋ
    case 'l': case 0x026B: case 0x026C: case 0x026D: case 0x026E:
        return "L";                     // l ɫ ɬ ɭ ɮ
    case 'r': case 0x0279: case 0x027A: case 0x027B: case 0x0280:
    case 0x0281:
        return "R";                     // r ɹ ɾ ɻ ʀ ʁ
    case 'j': case 0x029D: return "Y";  // j ʝ
    case 'w': case 0x028D: case 0x02AE: return "W";  // w ʍ ʷ
    case 0x03B2: return "B";            // β
    case 0x0278: return "F";            // ɸ
    case 0x028B: return "V";            // ʋ
    case 0x0263: case 0x0262: return "G";  // ɣ ɢ
    case 0x0265: return "Y";            // ɥ
    case 0x0270: return "W";            // ɰ
    case 0x0255: case 0x00E7: return "S";  // ç
    case 0x03C7: return "K";            // χ
    case 0x027E: case 0x027D: return "T";  // ɾ ɽ
    case 0x0294: case 0x0295: case 0x0296: case 0x02A1: case 0x02A2:
        return NULL;                    // glottal/epiglottal stops: silent
    default:
        return NULL;
    }
}

static bool ipa_emit(char *out, size_t cap, size_t *used, size_t *tokens,
                     uint8_t stress, const char *name)
{
    if (*tokens >= LS_PRON_TOKENS_MAX) return false;
    size_t len = strlen(name);
    size_t need = len + (*tokens ? 1 : 0) + (stress ? 1 : 0);
    if (*used + need + 1 > cap) return false;

    if (*tokens) out[(*used)++] = ' ';
    if (stress) out[(*used)++] = stress == 1 ? '\'' : ',';
    memcpy(out + *used, name, len);
    *used += len;
    out[*used] = '\0';
    (*tokens)++;
    return true;
}

size_t ls_ipa_to_arpabet(const char *ipa, char *out, size_t cap)
{
    if (!ipa || !out || cap < 2) return 0;
    out[0] = '\0';

    uint32_t cp[IPA_MAX_CODEPOINTS];
    size_t n = ipa_decode(ipa, strlen(ipa), cp, IPA_MAX_CODEPOINTS);
    if (n == 0) return 0;

    size_t used = 0;
    size_t tokens = 0;
    size_t vowels = 0;
    uint8_t stress = 0;

    for (size_t i = 0; i < n; i++) {
        uint32_t c = cp[i];
        uint32_t next = (i + 1 < n) ? cp[i + 1] : 0;
        uint32_t next2 = (i + 2 < n) ? cp[i + 2] : 0;

        // Stress marks and the separators dictionaries wrap a transcription
        // in. ARPAbet's own leading ' and , are accepted as stress too.
        if (c == 0x02C8 || c == '\'') {
            stress = 1;
            continue;
        }
        if (c == 0x02CC || c == ',') {
            stress = 2;
            continue;
        }
        if (c == 0x02D0 || c == 0x02D1 || c == ':' ||  // length marks
            c == 0x02B0 || c == 0x02B1 || c == 0x02B2 || c == 0x02B7 ||
            c == 0x0303 || c == 0x0308 || c == 0x032F || c == 0x0332 ||
            c == 0x031F || c == 0x0325) {
            continue;  // aspiration, nasalization, unreleased, etc.
        }
        if (c == '/' || c == '[' || c == ']' || c == '(' || c == ')' ||
            c == ' ' || c == '\t' || c == '.' || c == '-' || c == '|' ||
            c == 0x2016) {
            continue;
        }

        // Syllabic l/n/m/r: /ˈbʌtn̩/ should not lose the final syllable.
        if (next == 0x0329) {
            const char *base = NULL;
            bool syllabic_vowel = false;
            switch (c) {
            case 'l': base = "L"; break;
            case 'n': base = "N"; break;
            case 'm': base = "M"; break;
            case 'r': base = "ER"; syllabic_vowel = true; break;
            default: break;
            }
            if (base) {
                if (syllabic_vowel) {
                    if (!ipa_emit(out, cap, &used, &tokens, stress, base)) {
                        goto fail;
                    }
                    vowels++;
                } else {
                    if (!ipa_emit(out, cap, &used, &tokens, stress, "AH") ||
                        !ipa_emit(out, cap, &used, &tokens, 0, base)) {
                        goto fail;
                    }
                    vowels++;
                }
                stress = 0;
                i++;
                continue;
            }
        }

        // Affricates and /x/ before the one-symbol consonant table.
        if (c == 't' && next == 0x0283) {  // tʃ
            if (!ipa_emit(out, cap, &used, &tokens, 0, "CH")) goto fail;
            i++;
            continue;
        }
        if (c == 'd' && next == 0x0292) {  // dʒ
            if (!ipa_emit(out, cap, &used, &tokens, 0, "JH")) goto fail;
            i++;
            continue;
        }
        if (c == 'x') {  // ks
            if (!ipa_emit(out, cap, &used, &tokens, 0, "K") ||
                !ipa_emit(out, cap, &used, &tokens, 0, "S")) {
                goto fail;
            }
            continue;
        }

        if (ipa_is_vowel_cp(c)) {
            bool long_mark = (next == 0x02D0 || next == 0x02D1);
            size_t skip = long_mark ? 1 : 0;
            uint32_t after = (i + skip + 1 < n) ? cp[i + skip + 1] : 0;
            uint32_t after2 = (i + skip + 2 < n) ? cp[i + skip + 2] : 0;
            const char *v1 = NULL;
            const char *v2 = NULL;

            switch (c) {
            case 'a':
                if (next == 0x026A || next == 'i') { v1 = "AY"; skip = 1; }
                else if (next == 0x028A || next == 'u') { v1 = "AW"; skip = 1; }
                else v1 = "AE";
                break;
            case 'e':
                if (next == 0x026A || next == 'i') { v1 = "EY"; skip = 1; }
                else if (next == 0x0259) { v1 = "EH"; v2 = "R"; skip = 1; }
                else v1 = "EH";
                break;
            case 'i':
                v1 = "IY";
                break;
            case 'o':
                if (next == 0x028A || next == 'u') { v1 = "OW"; skip = 1; }
                else if (next == 0x026A || next == 'i') { v1 = "OY"; skip = 1; }
                else v1 = "OW";
                break;
            case 'u':
                v1 = "UW";
                break;
            case 0x026A:  // ɪ
                if (next == 0x0259) { v1 = "IH"; v2 = "R"; skip = 1; }
                else v1 = long_mark ? "IY" : "IH";
                break;
            case 0x028A:  // ʊ
                if (next == 0x0259) { v1 = "UH"; v2 = "R"; skip = 1; }
                else v1 = long_mark ? "UW" : "UH";
                break;
            case 0x0259:  // ə
                if (next == 'r' &&
                    (next2 == 0 || !ipa_is_vowel_cp(next2))) {
                    v1 = "ER";  // r-coloured schwa
                    skip = 1;
                } else if (next == 0x028A || next == 'u') {
                    v1 = "OW";  // BrE /əʊ/
                    skip = 1;
                } else {
                    v1 = "AH";
                }
                break;
            case 0x0254:  // ɔ
                if (next == 0x026A || next == 'i') { v1 = "OY"; skip = 1; }
                else v1 = "AO";
                break;
            case 0x025B:  // ɛ
                v1 = "EH";
                break;
            case 0x00E6:  // æ
                v1 = "AE";
                break;
            case 0x0251:  // ɑ
            case 0x0252:  // ɒ
                v1 = "AA";
                break;
            case 0x028C:  // ʌ
            case 0x0250:  // ɐ
            case 0x0264:  // ɤ
                v1 = "AH";
                break;
            case 0x025C:  // ɜ
            case 0x025D:  // ɝ
            case 0x025A:  // ɚ
            case 0x025E:  // ɞ
            case 0x00F8:  // ø
            case 0x0153:  // œ
                v1 = "ER";
                // ɜːr is one r-coloured vowel, not ER R.
                if (after == 'r' &&
                    (after2 == 0 || !ipa_is_vowel_cp(after2))) {
                    skip++;
                }
                break;
            case 0x0268:  // ɨ
                v1 = "IH";
                break;
            case 0x0289:  // ʉ
            case 0x026F:  // ɯ
                v1 = "UW";
                break;
            default:
                v1 = "AH";
                break;
            }

            if (!ipa_emit(out, cap, &used, &tokens, stress, v1)) goto fail;
            stress = 0;
            vowels++;
            if (v2 && !ipa_emit(out, cap, &used, &tokens, 0, v2)) goto fail;
            i += skip;
            continue;
        }

        const char *cons = ipa_consonant(c);
        if (cons && !ipa_emit(out, cap, &used, &tokens, 0, cons)) goto fail;
        // Anything else (tone digits, combining marks, unknown symbols) is
        // ignored; a usable transcription usually survives one odd glyph.
    }

    if (vowels == 0 || used == 0) goto fail;
    if (!ls_pron_validate(out)) goto fail;
    return used;

fail:
    out[0] = '\0';
    return 0;
}

// IPA spelling of each phoneme. Only unstressed AH is stress-dependent and is
// handled in ls_pron_to_ipa(); every other symbol is fixed. These strings are
// UTF-8 so the UI renders them with langstudy_ipa_16.
//
// Two substitutions keep the line readable on a 16 px monochrome-ish display:
//   * The near-close front vowel is written as plain "i" rather than U+026A
//     (ɪ). At this size the small-capital I glyph is a featureless bar that
//     learners read as a missing character, especially inside the AY/OY
//     diphthongs. IY still carries the length mark (iː) so long and short i
//     remain distinct.
//   * ER is always written "ər" (schwa + r) rather than U+025D/U+025A
//     (ɝ/ɚ). Those rhotic schwa glyphs are only 9 px tall and their wide,
//     curled shape smears into a bar on the panel, which is exactly what
//     "desert" and "glacier" expose. "ər" is also the spelling most learners
//     already recognise.
static const char *ipa_symbol(ls_phoneme_t phoneme)
{
    switch (phoneme) {
    case LS_PH_AA: return "\xC9\x91";            // ɑ
    case LS_PH_AE: return "\xC3\xA6";            // æ
    case LS_PH_AH: return "\xCA\x8C";            // ʌ (unstressed: ə)
    case LS_PH_AO: return "\xC9\x94";            // ɔ
    case LS_PH_AW: return "a\xCA\x8A";           // aʊ
    case LS_PH_AY: return "ai";                  // aɪ, simplified for legibility
    case LS_PH_EH: return "\xC9\x9B";            // ɛ
    case LS_PH_ER: return "\xC9\x99r";           // ər (learner-friendly)
    case LS_PH_EY: return "ei";                  // eɪ, simplified for legibility
    case LS_PH_IH: return "i";                   // ɪ, simplified for legibility
    case LS_PH_IY: return "i\xCB\x90";           // iː
    case LS_PH_OW: return "o\xCA\x8A";           // oʊ
    case LS_PH_OY: return "\xC9\x94i";           // ɔɪ, simplified for legibility
    case LS_PH_UH: return "\xCA\x8A";            // ʊ
    case LS_PH_UW: return "u\xCB\x90";           // uː
    case LS_PH_B: return "b";
    case LS_PH_CH: return "t\xCA\x83";           // tʃ
    case LS_PH_D: return "d";
    case LS_PH_DH: return "\xC3\xB0";            // ð
    case LS_PH_F: return "f";
    case LS_PH_G: return "\xC9\xA1";             // ɡ
    case LS_PH_HH: return "h";
    case LS_PH_JH: return "d\xCA\x92";           // dʒ
    case LS_PH_K: return "k";
    case LS_PH_L: return "l";
    case LS_PH_M: return "m";
    case LS_PH_N: return "n";
    case LS_PH_NG: return "\xC5\x8B";            // ŋ
    case LS_PH_P: return "p";
    case LS_PH_R: return "r";
    case LS_PH_S: return "s";
    case LS_PH_SH: return "\xCA\x83";            // ʃ
    case LS_PH_T: return "t";
    case LS_PH_TH: return "\xCE\xB8";            // θ
    case LS_PH_V: return "v";
    case LS_PH_W: return "w";
    case LS_PH_Y: return "j";
    case LS_PH_Z: return "z";
    case LS_PH_ZH: return "\xCA\x92";            // ʒ
    default: return NULL;
    }
}

static size_t ipa_put(char *out, size_t cap, size_t used, const char *text)
{
    size_t len = strlen(text);
    if (used + len >= cap) return 0;
    memcpy(out + used, text, len);
    return used + len;
}

static bool ipa_is_vowel(ls_phoneme_t phoneme)
{
    return (unsigned)phoneme <= (unsigned)LS_PH_UW;
}

size_t ls_pron_to_ipa(const char *pron, char *out, size_t cap)
{
    if (!pron || !out || cap < 3) return 0;
    out[0] = '\0';
    if (pron[0] == '\0') return 0;

    ls_phoneme_t phonemes[LS_PRON_TOKENS_MAX];
    uint8_t stress[LS_PRON_TOKENS_MAX];
    size_t count = ls_pron_tokenize(pron, phonemes, stress, LS_PRON_TOKENS_MAX);
    if (count == 0) return 0;

    // Put each stress mark at the onset of the syllable holding the stressed
    // vowel, not directly before the vowel. English onsets are maximized: of
    // the consonants between two vowels, only the first stays in the previous
    // coda. "K AH0 M P Y UW1 T ER0" therefore becomes /kəmˈpjuːtər/ rather than
    // the misleading /kəmpjˈuːtər/.
    uint8_t mark_at[LS_PRON_TOKENS_MAX] = {0};
    size_t prev_vowel = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (!ipa_is_vowel(phonemes[i])) continue;
        if (stress[i] != 0) {
            size_t onset;
            if (prev_vowel == SIZE_MAX) {
                onset = 0;  // leading consonants, or the vowel itself
            } else {
                size_t gap = i - prev_vowel - 1;
                onset = prev_vowel + (gap > 1 ? gap - 1 : 1);
                if (onset > i) onset = i;
            }
            mark_at[onset] = stress[i];
        }
        prev_vowel = i;
    }

    size_t used = 0;
    out[used++] = '/';
    for (size_t i = 0; i < count; i++) {
        if (mark_at[i] != 0) {
            used = ipa_put(out, cap, used,
                           mark_at[i] == 1 ? "\xCB\x88" : "\xCB\x8C");
            if (used == 0) goto fail;
        }
        const char *symbol = ipa_symbol(phonemes[i]);
        if (!symbol) goto fail;
        // Unstressed AH uses the reduced vowel. ER is "ər" at every stress
        // level, so it needs no override here.
        if (stress[i] == 0 && phonemes[i] == LS_PH_AH) symbol = "\xC9\x99";
        used = ipa_put(out, cap, used, symbol);
        if (used == 0) goto fail;
    }

    if (used + 2 > cap) goto fail;
    out[used++] = '/';
    out[used] = '\0';
    return used;

fail:
    out[0] = '\0';
    return 0;
}
