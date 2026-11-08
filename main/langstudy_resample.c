// main/langstudy_resample.c
#include "langstudy_resample.h"

bool ls_resampler_init(ls_resampler_t *state, uint32_t in_rate,
                       uint32_t out_rate)
{
    if (!state || in_rate == 0 || out_rate == 0) return false;
    state->step_q16 = ((uint64_t)in_rate << 16) / out_rate;
    if (state->step_q16 == 0) return false;
    state->pos_q16 = 0;
    state->index = 0;
    state->previous = 0;
    state->started = false;
    return true;
}

size_t ls_resampler_process(ls_resampler_t *state, const int16_t *in,
                            size_t in_count, int16_t *out, size_t out_cap)
{
    if (!state || !in || !out || out_cap == 0) return 0;
    size_t written = 0;

    for (size_t i = 0; i < in_count; i++) {
        int16_t sample = in[i];

        if (!state->started) {
            state->started = true;
            state->previous = sample;
            // The stream starts at input index 0, so the first output is exact.
            out[written++] = sample;
            state->pos_q16 += state->step_q16;
            state->index = 1;
            continue;
        }

        // pos_q16 is an absolute input position. Interpolate between the
        // previous sample (index-1) and this one (index).
        uint64_t here = state->index << 16;
        uint64_t previous_pos = here - 65536;
        while (state->pos_q16 <= here) {
            if (written >= out_cap) return written;
            uint32_t fraction = (uint32_t)(state->pos_q16 - previous_pos);
            int32_t delta = (int32_t)sample - state->previous;
            int32_t value =
                state->previous + (int32_t)(((int64_t)delta * fraction) >> 16);
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            out[written++] = (int16_t)value;
            state->pos_q16 += state->step_q16;
        }
        state->previous = sample;
        state->index++;
    }
    return written;
}
