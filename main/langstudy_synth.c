// main/langstudy_synth.c
#include "langstudy_synth.h"

#include "langstudy_phoneme.h"

#include <math.h>
#include <string.h>

#define TTS_FS 16000.0f
#define TTS_PI 3.14159265358979323846f
#define TTS_FORMANT_ALPHA 0.018f      // ~3.5 ms; settles inside one phone
#define TTS_DIPHTHONG_ALPHA 0.0065f   // slower glide for the second half
#define TTS_COEFF_PERIOD 64
#define TTS_MASTER_GAIN 0.70f
#define TTS_GLOTTAL_GAIN 1.50f
#define TTS_TILT_COEFF 0.22f          // glottal spectral roll-off (~-12 dB/oct)

enum {
    KIND_VOICED = 0,
    KIND_NOISE,
    KIND_VOICED_NOISE,
    KIND_STOP,
    KIND_AFFRICATE,
};

typedef struct {
    uint8_t kind;
    uint8_t amp;      // 0..100
    uint8_t voiced;
    uint8_t closure_ms;
    uint16_t f1, f2, f3;
    uint16_t e1, e2, e3; // diphthong end targets, 0 when monophthong
    uint16_t dur_ms;
} phoneme_info_t;

// Formant targets are classic American-English values. Stops use the burst
// locus; affricates use the fricative portion after a short closure.
static const phoneme_info_t s_info[LS_PHONEME_COUNT] = {
    [LS_PH_AA] = {KIND_VOICED, 100, 1, 0, 730, 1090, 2440, 0, 0, 0, 120},
    [LS_PH_AE] = {KIND_VOICED, 100, 1, 0, 660, 1720, 2410, 0, 0, 0, 120},
    [LS_PH_AH] = {KIND_VOICED, 90, 1, 0, 640, 1190, 2390, 0, 0, 0, 100},
    [LS_PH_AO] = {KIND_VOICED, 100, 1, 0, 570, 840, 2410, 0, 0, 0, 120},
    [LS_PH_AW] = {KIND_VOICED, 100, 1, 0, 730, 1090, 2440, 440, 1020, 2240, 170},
    [LS_PH_AY] = {KIND_VOICED, 100, 1, 0, 730, 1090, 2440, 270, 2290, 3010, 170},
    [LS_PH_EH] = {KIND_VOICED, 100, 1, 0, 530, 1840, 2480, 0, 0, 0, 110},
    [LS_PH_ER] = {KIND_VOICED, 95, 1, 0, 490, 1350, 1690, 0, 0, 0, 120},
    [LS_PH_EY] = {KIND_VOICED, 100, 1, 0, 530, 1840, 2480, 270, 2290, 3010, 170},
    [LS_PH_IH] = {KIND_VOICED, 95, 1, 0, 390, 1990, 2550, 0, 0, 0, 100},
    [LS_PH_IY] = {KIND_VOICED, 100, 1, 0, 270, 2290, 3010, 0, 0, 0, 110},
    [LS_PH_OW] = {KIND_VOICED, 100, 1, 0, 570, 840, 2410, 300, 870, 2240, 170},
    [LS_PH_OY] = {KIND_VOICED, 100, 1, 0, 570, 840, 2410, 270, 2290, 3010, 180},
    [LS_PH_UH] = {KIND_VOICED, 90, 1, 0, 440, 1020, 2240, 0, 0, 0, 100},
    [LS_PH_UW] = {KIND_VOICED, 95, 1, 0, 300, 870, 2240, 0, 0, 0, 110},

    [LS_PH_B] = {KIND_STOP, 80, 1, 40, 900, 1500, 2500, 0, 0, 0, 65},
    [LS_PH_CH] = {KIND_AFFRICATE, 70, 0, 30, 2000, 3000, 4000, 0, 0, 0, 100},
    [LS_PH_D] = {KIND_STOP, 80, 1, 35, 3000, 4000, 5000, 0, 0, 0, 60},
    [LS_PH_DH] = {KIND_VOICED_NOISE, 45, 1, 0, 3000, 4000, 5000, 0, 0, 0, 60},
    [LS_PH_F] = {KIND_NOISE, 40, 0, 0, 3500, 5000, 6500, 0, 0, 0, 90},
    [LS_PH_G] = {KIND_STOP, 80, 1, 40, 1500, 2200, 3000, 0, 0, 0, 70},
    [LS_PH_HH] = {KIND_NOISE, 35, 0, 0, 1200, 2200, 3000, 0, 0, 0, 60},
    [LS_PH_JH] = {KIND_AFFRICATE, 65, 1, 30, 2000, 3000, 4000, 0, 0, 0, 100},
    [LS_PH_K] = {KIND_STOP, 80, 0, 45, 1500, 2500, 3500, 0, 0, 0, 75},
    [LS_PH_L] = {KIND_VOICED, 75, 1, 0, 360, 1300, 2700, 0, 0, 0, 70},
    [LS_PH_M] = {KIND_VOICED, 60, 1, 0, 250, 1000, 2200, 0, 0, 0, 70},
    [LS_PH_N] = {KIND_VOICED, 60, 1, 0, 250, 1500, 2500, 0, 0, 0, 70},
    [LS_PH_NG] = {KIND_VOICED, 60, 1, 0, 250, 2000, 2500, 0, 0, 0, 75},
    [LS_PH_P] = {KIND_STOP, 75, 0, 40, 800, 1200, 2200, 0, 0, 0, 70},
    [LS_PH_R] = {KIND_VOICED, 75, 1, 0, 400, 1200, 1600, 0, 0, 0, 70},
    [LS_PH_S] = {KIND_NOISE, 45, 0, 0, 4000, 6000, 7000, 0, 0, 0, 95},
    [LS_PH_SH] = {KIND_NOISE, 45, 0, 0, 2000, 3000, 4000, 0, 0, 0, 95},
    [LS_PH_T] = {KIND_STOP, 75, 0, 35, 3000, 4000, 5000, 0, 0, 0, 65},
    [LS_PH_TH] = {KIND_NOISE, 32, 0, 0, 3500, 5000, 6500, 0, 0, 0, 80},
    [LS_PH_V] = {KIND_VOICED_NOISE, 50, 1, 0, 3000, 4000, 5000, 0, 0, 0, 70},
    [LS_PH_W] = {KIND_VOICED, 70, 1, 0, 300, 600, 2200, 0, 0, 0, 60},
    [LS_PH_Y] = {KIND_VOICED, 70, 1, 0, 260, 2070, 3020, 0, 0, 0, 60},
    [LS_PH_Z] = {KIND_VOICED_NOISE, 50, 1, 0, 4000, 6000, 7000, 0, 0, 0, 85},
    [LS_PH_ZH] = {KIND_VOICED_NOISE, 50, 1, 0, 2000, 3000, 4000, 0, 0, 0, 85},
};

// One resonator section. `b0` normalizes the response peak to unity so a
// 300 Hz band and a 4 kHz band have comparable loudness. Using `1 - r` (the
// obvious choice) leaves a peak gain of several times unity for low formants,
// which made /l/, /r/ and /w/ much louder than vowels and clipped the output.
static void section_coeffs(float f, float bandwidth, float *a1, float *a2,
                           float *b0)
{
    if (f < 100.0f) f = 100.0f;
    if (f > 7400.0f) f = 7400.0f;
    float r = expf(-TTS_PI * bandwidth / TTS_FS);
    float theta = 2.0f * TTS_PI * f / TTS_FS;
    *a1 = 2.0f * r * cosf(theta);
    *a2 = -r * r;
    float den = 1.0f - 2.0f * r * cosf(2.0f * theta) + r * r;
    if (den < 1e-6f) den = 1e-6f;
    *b0 = (1.0f - r) * sqrtf(den);
}

static void update_coefficients(ls_tts_t *tts)
{
    // Vowels need narrow formants to stay distinct. Fricatives need wide bands:
    // a 90 Hz band around 4-7 kHz turns hiss into a whistle, which is a large
    // part of why the old voice was hard to understand.
    static const float bw_voiced[3] = {90.0f, 110.0f, 150.0f};
    static const float bw_noise[3] = {900.0f, 1300.0f, 1700.0f};
    static const float bw_voiced_noise[3] = {350.0f, 600.0f, 900.0f};
    static const float bw_burst[3] = {300.0f, 450.0f, 650.0f};

    const float *bw = bw_voiced;
    if (tts->kind == KIND_NOISE) bw = bw_noise;
    else if (tts->kind == KIND_VOICED_NOISE) bw = bw_voiced_noise;
    else if (tts->kind == KIND_STOP || tts->kind == KIND_AFFRICATE) bw = bw_burst;

    for (int i = 0; i < 3; i++) {
        section_coeffs(tts->f_cur[i], bw[i], &tts->a1[i], &tts->a2[i],
                       &tts->b0[i]);
    }
    section_coeffs(tts->f4_cur, 260.0f, &tts->a1_4, &tts->a2_4, &tts->b0_4);
}

static void set_targets(ls_tts_t *tts, float f1, float f2, float f3)
{
    tts->f_target[0] = f1;
    tts->f_target[1] = f2;
    tts->f_target[2] = f3;
    tts->f4_target = 3500.0f;
}

static void start_segment(ls_tts_t *tts)
{
    uint8_t id = tts->phoneme[tts->index];
    const phoneme_info_t *info = &s_info[id];
    bool vowel = id <= LS_PH_UW;
    bool last = tts->index + 1 >= tts->count;
    uint32_t duration_ms = info->dur_ms;

    if (vowel) {
        if (tts->stress[tts->index] == 1) duration_ms = duration_ms * 125 / 100;
        else if (tts->stress[tts->index] == 2) duration_ms = duration_ms * 112 / 100;
        // Final lengthening is a strong cue that a word has ended.
        if (last) duration_ms = duration_ms * 112 / 100;
    }

    tts->length = duration_ms * LS_TTS_SAMPLE_RATE / 1000;
    if (tts->length == 0) tts->length = 1;
    tts->closure = (uint32_t)info->closure_ms * LS_TTS_SAMPLE_RATE / 1000;
    if (tts->closure >= tts->length) tts->closure = tts->length / 2;

    tts->kind = info->kind;
    tts->voiced = info->voiced;
    tts->amp = info->amp;
    tts->pos = 0;
    tts->diphthong_end = info->e1 != 0;
    tts->formant_alpha = TTS_FORMANT_ALPHA;
    set_targets(tts, info->f1, info->f2, info->f3);

    // Fade only where the next phone is silent or noisy. Fading between two
    // voiced phones would put a dip in the middle of a syllable; the formant
    // glide already keeps that transition smooth.
    uint32_t attack = 60;
    uint32_t release = 0;
    if (last) {
        release = 300;
    } else {
        const phoneme_info_t *next = &s_info[tts->phoneme[tts->index + 1]];
        if (next->kind == KIND_STOP || next->kind == KIND_AFFRICATE) {
            release = next->voiced ? 100 : 190;
        } else if (next->kind == KIND_NOISE) {
            release = 150;
        }
    }
    tts->env_attack = attack;
    tts->env_release = release;

    bool first = !tts->started;
    if (first) {
        tts->f_cur[0] = info->f1;
        tts->f_cur[1] = info->f2;
        tts->f_cur[2] = info->f3;
        tts->f4_cur = 3500.0f;
        tts->started = true;
    }

    float base = 110.0f + (float)(tts->seed % 35u);
    if (tts->stress[tts->index] == 1) base *= 1.18f;
    else if (tts->stress[tts->index] == 2) base *= 1.10f;
    float contour = 1.0f - 0.10f * ((float)tts->index / (float)tts->count);
    if (last) contour *= 0.92f;
    tts->f0_target = base * contour;
    if (first) tts->f0_cur = tts->f0_target;

    tts->coeff_timer = TTS_COEFF_PERIOD - 1;
    update_coefficients(tts);
}

static float next_noise(ls_tts_t *tts)
{
    uint32_t x = tts->noise;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    tts->noise = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static float segment_envelope(uint32_t pos, uint32_t length,
                              uint32_t attack, uint32_t release)
{
    if (attack && pos < attack) return (float)pos / (float)attack;
    if (release && pos + release >= length) {
        uint32_t left = length > pos ? length - pos : 0;
        float env = (float)left / (float)release;
        return env > 1.0f ? 1.0f : env;
    }
    return 1.0f;
}

// A raw sawtooth has only a -6 dB/octave roll-off, which the ear hears as a
// buzzy "robot" tone. A one-pole low-pass brings it closer to the -12 dB/octave
// glottal source of a real voice; a little aspiration noise adds breath.
static float glottal_source(ls_tts_t *tts, float noise)
{
    float saw = 2.0f * tts->phase - 1.0f;
    tts->glottal_lp += (saw - tts->glottal_lp) * TTS_TILT_COEFF;
    return tts->glottal_lp * TTS_GLOTTAL_GAIN + noise * 0.02f;
}

static float render_sample(ls_tts_t *tts)
{
    bool stopish = (tts->kind == KIND_STOP || tts->kind == KIND_AFFRICATE);
    bool closure = stopish && tts->pos < tts->closure;
    float output = 0.0f;

    if (!closure) {
        float noise = next_noise(tts);
        float glottal = glottal_source(tts, noise);
        float source;
        switch (tts->kind) {
        case KIND_VOICED:
            source = glottal;
            break;
        case KIND_VOICED_NOISE:
            source = glottal * 0.55f + noise * 0.55f;
            break;
        case KIND_STOP:
            source = tts->voiced ? glottal * 0.45f + noise * 0.65f : noise;
            break;
        case KIND_AFFRICATE:
            source = tts->voiced ? glottal * 0.35f + noise * 0.75f : noise;
            break;
        default: // KIND_NOISE
            source = noise;
            break;
        }

        float env = segment_envelope(tts->pos, tts->length, tts->env_attack,
                                     tts->env_release);
        if (stopish) {
            // Closure is silent, then a short burst tuned to the place of
            // articulation, then decaying aspiration (or a short voiced
            // release for /b d g/). This is what makes stops audible instead
            // of sounding like clicks.
            uint32_t burst = (tts->kind == KIND_AFFRICATE ? 10u : 7u) *
                             LS_TTS_SAMPLE_RATE / 1000u;
            uint32_t tail = tts->length > tts->closure
                                ? tts->length - tts->closure
                                : 0;
            uint32_t since = tts->pos >= tts->closure
                                 ? tts->pos - tts->closure
                                 : 0;
            if (tail == 0) {
                env = 0.0f;
            } else if (since < burst) {
                env = 1.15f;
            } else {
                float t = (float)(since - burst) /
                          (float)(tail > burst ? tail - burst : 1);
                if (t > 1.0f) t = 1.0f;
                env = (1.0f - t) * (1.0f - t) *
                      (tts->voiced ? 0.28f : 0.62f);
            }
        }

        static const float weight[3] = {1.0f, 0.55f, 0.30f};
        float sum = 0.0f;
        for (int i = 0; i < 3; i++) {
            float y = tts->b0[i] * source +
                      tts->a1[i] * tts->y1[i] +
                      tts->a2[i] * tts->y2[i];
            tts->y2[i] = tts->y1[i];
            tts->y1[i] = y;
            sum += y * weight[i];
        }

        float y4 = tts->b0_4 * source + tts->a1_4 * tts->y1_4 +
                   tts->a2_4 * tts->y2_4;
        tts->y2_4 = tts->y1_4;
        tts->y1_4 = y4;
        // The fourth formant is a voice-timbre cue; hiss does not need it.
        if (tts->kind != KIND_NOISE) sum += y4 * 0.20f;

        // Fricatives need a broadband component as well as the shaped bands,
        // or they come out as faint whistles.
        if (tts->kind == KIND_NOISE) {
            sum = sum * 0.55f + noise * 0.45f;
        } else if (tts->kind == KIND_VOICED_NOISE) {
            sum = sum * 0.80f + noise * 0.25f;
        }

        output = sum * ((float)tts->amp / 100.0f) * env * TTS_MASTER_GAIN;
    }

    for (int i = 0; i < 3; i++) {
        tts->f_cur[i] +=
            (tts->f_target[i] - tts->f_cur[i]) * tts->formant_alpha;
    }
    tts->f4_cur += (tts->f4_target - tts->f4_cur) * tts->formant_alpha;
    if (++tts->coeff_timer >= TTS_COEFF_PERIOD) {
        update_coefficients(tts);
        tts->coeff_timer = 0;
    }

    // Slow pitch drift makes stressed and unstressed syllables join smoothly
    // instead of stepping, and a tiny jitter breaks up the perfect periodicity
    // that reads as synthetic.
    tts->f0_cur += (tts->f0_target - tts->f0_cur) * 0.003f;
    if (++tts->jitter_timer >= 64) {
        tts->jitter_timer = 0;
        tts->jitter = 1.0f + next_noise(tts) * 0.008f;
    }
    tts->phase += tts->f0_cur * tts->jitter / TTS_FS;
    if (tts->phase >= 1.0f) tts->phase -= 1.0f;
    if (tts->phase < 0.0f) tts->phase += 1.0f;

    // Remove DC/rumble before quantizing; a DC offset both wastes headroom and
    // produces a click when playback starts or stops.
    float y = output - tts->hp_x1 + 0.995f * tts->hp_y1;
    tts->hp_x1 = output;
    tts->hp_y1 = y;
    return y;
}

bool ls_tts_start(ls_tts_t *tts, const char *pron, uint32_t seed)
{
    if (!tts || !pron) return false;

    ls_phoneme_t phonemes[LS_PRON_TOKENS_MAX];
    uint8_t stress[LS_PRON_TOKENS_MAX];
    size_t count = ls_pron_tokenize(pron, phonemes, stress, LS_PRON_TOKENS_MAX);
    if (count == 0 || count > LS_PRON_TOKENS_MAX) return false;

    memset(tts, 0, sizeof(*tts));
    uint32_t total_ms = 0;
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        const phoneme_info_t *info = &s_info[phonemes[i]];
        uint32_t duration = info->dur_ms;
        if (phonemes[i] <= LS_PH_UW) {
            if (stress[i] == 1) duration = duration * 125 / 100;
            else if (stress[i] == 2) duration = duration * 112 / 100;
        }
        if (total_ms + duration > LS_TTS_MAX_DURATION_MS) break;
        total_ms += duration;
        tts->phoneme[kept] = (uint8_t)phonemes[i];
        tts->stress[kept] = stress[i];
        kept++;
    }
    if (kept == 0) return false;

    tts->count = (uint8_t)kept;
    tts->index = 0;
    tts->pos = 0;
    tts->length = 0;
    tts->seed = seed;
    tts->noise = seed ? seed : 0x9E3779B9u;
    tts->jitter = 1.0f;
    tts->done = false;
    tts->started = false;
    return true;
}

size_t ls_tts_render(ls_tts_t *tts, int16_t *out, size_t max_samples)
{
    if (!tts || !out || max_samples == 0) return 0;

    size_t written = 0;
    while (written < max_samples && !tts->done) {
        if (tts->length == 0) {
            if (tts->index >= tts->count) {
                tts->done = true;
                break;
            }
            start_segment(tts);
        }

        if (tts->diphthong_end && tts->pos >= tts->length / 2) {
            const phoneme_info_t *info = &s_info[tts->phoneme[tts->index]];
            set_targets(tts, info->e1, info->e2, info->e3);
            // Glide over the second half rather than snapping to the end
            // target, which is what makes /aɪ/ and /eɪ/ sound like one vowel.
            tts->formant_alpha = TTS_DIPHTHONG_ALPHA;
            tts->diphthong_end = false;
        }

        float sample = render_sample(tts);
        int32_t value = (int32_t)(sample * 32767.0f);
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        out[written++] = (int16_t)value;

        tts->pos++;
        if (tts->pos >= tts->length) {
            tts->index++;
            tts->length = 0;
            if (tts->index >= tts->count) tts->done = true;
        }
    }
    return written;
}

bool ls_tts_done(const ls_tts_t *tts)
{
    return tts ? tts->done : true;
}
