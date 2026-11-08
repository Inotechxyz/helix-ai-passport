// main/langstudy_resample.h
// Streaming linear-interpolation resampler for 16-bit mono PCM. Edge TTS
// returns 24 kHz audio while the ES8311 playback path runs at 16 kHz.
//
// One input sample of latency is unavoidable: an output position between input
// samples n-1 and n can only be interpolated after n arrives. The state is
// small enough to keep on the stack.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t step_q16;  // input samples per output sample, Q16
    uint64_t pos_q16;   // next output position, Q16, absolute stream index
    uint64_t index;     // number of input samples consumed
    int16_t previous;
    bool started;
} ls_resampler_t;

// Prepare for in_rate -> out_rate. Returns false for a nonsensical rate.
bool ls_resampler_init(ls_resampler_t *state, uint32_t in_rate,
                       uint32_t out_rate);

// Resample in_count samples into out. out_cap must be at least
// (in_count * out_rate / in_rate) + 2; the return value is the number of
// samples written (0 only when out_cap is zero).
size_t ls_resampler_process(ls_resampler_t *state, const int16_t *in,
                            size_t in_count, int16_t *out, size_t out_cap);
