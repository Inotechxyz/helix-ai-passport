// main/langstudy_synth.h
// Small streaming formant synthesizer for offline pronunciation.
//
// A spectrally tilted glottal source and a noise source drive four resonators,
// with per-phoneme formant targets, wider bandwidths for fricatives, stop
// bursts and aspiration, and stress/pitch/duration prosody. Output is 16 kHz,
// signed 16-bit, mono, generated in caller-sized chunks so no multi-kilobyte
// PCM buffer has to be allocated.
//
// This is still a formant synthesizer, not a recorded human voice: an ESP32-C3
// has no room for a neural TTS model. The goal here is clear, natural-sounding
// pronunciation rather than a studio recording.
#pragma once

#include "langstudy_model.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_TTS_SAMPLE_RATE 16000
#define LS_TTS_MAX_DURATION_MS 3000

typedef struct {
    uint8_t phoneme[LS_PRON_TOKENS_MAX];
    uint8_t stress[LS_PRON_TOKENS_MAX];
    uint8_t count;
    uint8_t index;
    uint8_t kind;
    uint8_t voiced;
    uint8_t amp;
    bool diphthong_end;
    bool started;
    bool done;

    uint32_t seed;
    uint32_t noise;
    uint32_t pos;
    uint32_t length;
    uint32_t closure;
    uint32_t coeff_timer;

    float f_target[3];
    float f_cur[3];
    float a1[3];
    float a2[3];
    float b0[3];
    float y1[3];
    float y2[3];

    // Fixed fourth formant adds the upper spectral peak that makes vowels sound
    // like a voice instead of a two-tone buzz.
    float f4_cur;
    float f4_target;
    float a1_4, a2_4, b0_4;
    float y1_4, y2_4;

    float glottal_lp;   // spectral tilt (one-pole low-pass state)
    float hp_x1, hp_y1; // output DC blocker

    float phase;
    float f0;
    float f0_cur;
    float f0_target;
    float jitter;
    float formant_alpha;
    uint32_t jitter_timer;
    uint32_t env_attack;
    uint32_t env_release;
} ls_tts_t;

// Prepare a pronunciation. Returns false for an empty/invalid pronunciation.
bool ls_tts_start(ls_tts_t *tts, const char *pron, uint32_t seed);

// Render up to max_samples into out. Returns the number of samples written;
// when fewer than max_samples are returned, ls_tts_done() is true.
size_t ls_tts_render(ls_tts_t *tts, int16_t *out, size_t max_samples);

bool ls_tts_done(const ls_tts_t *tts);
