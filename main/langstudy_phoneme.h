// main/langstudy_phoneme.h
// ARPAbet pronunciation tokenizer used by the offline speech synthesizer.
#pragma once

#include "langstudy_model.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LS_PH_AA, LS_PH_AE, LS_PH_AH, LS_PH_AO, LS_PH_AW, LS_PH_AY,
    LS_PH_EH, LS_PH_ER, LS_PH_EY, LS_PH_IH, LS_PH_IY, LS_PH_OW,
    LS_PH_OY, LS_PH_UH, LS_PH_UW,
    LS_PH_B, LS_PH_CH, LS_PH_D, LS_PH_DH, LS_PH_F, LS_PH_G,
    LS_PH_HH, LS_PH_JH, LS_PH_K, LS_PH_L, LS_PH_M, LS_PH_N,
    LS_PH_NG, LS_PH_P, LS_PH_R, LS_PH_S, LS_PH_SH, LS_PH_T,
    LS_PH_TH, LS_PH_V, LS_PH_W, LS_PH_Y, LS_PH_Z, LS_PH_ZH,
    LS_PHONEME_COUNT
} ls_phoneme_t;

// Parse one token such as "HH", "AH0", "'OW" or ",IY". Case-insensitive.
// A leading apostrophe marks primary stress and a comma secondary stress; a
// trailing digit (0..2) is also accepted, matching common ARPAbet dictionaries.
bool ls_phoneme_parse(const char *token, size_t len,
                      ls_phoneme_t *out, uint8_t *stress);

// Convert a space-separated pronunciation into phonemes. Returns the number of
// tokens written, or 0 when the input is empty or invalid. `stress` may be NULL.
size_t ls_pron_tokenize(const char *pron, ls_phoneme_t *phonemes,
                        uint8_t *stress, size_t cap);

// True for an empty pronunciation (the word simply has no TTS) or a valid one
// within the LS_PRON_MAX / LS_PRON_TOKENS_MAX limits.
bool ls_pron_validate(const char *pron);

// Worst-case display size. Each of the 32 tokens can become a 2-byte stress
// mark plus a 4-byte diphthong plus a separator; 192 bytes is ample for the
// 48-byte LS_PRON_MAX input and keeps the UI buffer small.
#define LS_IPA_TEXT_MAX 192

// Convert an ARPAbet pronunciation to an IPA string wrapped in slashes, e.g.
// "HH AH L 'OW" -> "/həˈloʊ/". Stress marks become ˈ (primary) and ˌ
// (secondary) at the syllable onset; unstressed AH becomes ə. For legibility
// at 16 px ER is always written "ər" instead of ɝ/ɚ, and the near-close front
// vowel is written as plain "i" instead of ɪ, so AY/OY appear as "ai"/"ɔi"
// and IH as "i".
// Returns the byte length written (excluding the NUL), or 0 for an
// empty/invalid pronunciation or when the result does not fit `cap`. The
// output is UTF-8.
size_t ls_pron_to_ipa(const char *pron, char *out, size_t cap);

// Convert an IPA transcription to space-separated ARPAbet. This is a fallback
// for model replies that ignore the prompt's ARPAbet request: the pack
// sanitizer would otherwise drop every non-ASCII symbol and leave the word
// silent. Handles the common English IPA inventory, diphthongs, affricates,
// length marks, syllabic consonants and stress placement on the syllable
// onset. Slashes/brackets around the transcription are optional. Returns the
// byte length written (excluding the NUL), or 0 when the input cannot be
// converted into a valid pronunciation that fits `cap`/LS_PRON_TOKENS_MAX.
size_t ls_ipa_to_arpabet(const char *ipa, char *out, size_t cap);

const char *ls_phoneme_name(ls_phoneme_t phoneme);
