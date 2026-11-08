// main/langstudy_audio.h
// Non-blocking pronunciation playback. A single worker renders the streaming
// formant synthesizer straight into bsp_audio_write(), so no PCM buffer is
// allocated per word and the UI/input task never blocks on I2S.
//
// The I2S/codec hardware is opened lazily on the first word. Call
// ls_audio_release() before starting Wi-Fi/TLS to return that internal RAM,
// then ls_audio_resume() afterwards. While suspended, speak requests are
// dropped.
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

esp_err_t ls_audio_init(void);
void ls_audio_shutdown(void);

// Queue a pronunciation (ARPAbet). Returns false when suspended, invalid, or
// the single-slot queue is busy.
bool ls_audio_speak(const char *pron);

void ls_audio_stop(void);

// Free codec + I2S resources. Waits up to 2 s for the current word to finish
// and is idempotent.
esp_err_t ls_audio_release(void);

// Allow playback again after ls_audio_release().
void ls_audio_resume(void);

bool ls_audio_is_speaking(void);

// Streaming playback for callers that decode audio themselves (the online Edge
// voice). begin() opens the codec/I2S and takes ownership for the session:
// queued offline pronunciations are dropped until end(). write() is safe to
// call from a worker task; it returns ESP_ERR_INVALID_STATE once the session
// was cancelled (ls_audio_stop/release) or never started. end() releases
// ownership and is idempotent.
esp_err_t ls_audio_stream_begin(void);
// Same as begin(), but opens the codec at sample_rate_hz. The Edge voice arrives
// natively at 24 kHz; playing it at its own rate avoids resampling it to the
// 16 kHz the offline synthesizer uses (the linear resampler left a wideband
// hiss around 4-8 kHz on top of the voice).
esp_err_t ls_audio_stream_begin_rate(uint32_t sample_rate_hz);
esp_err_t ls_audio_stream_write(const int16_t *pcm, size_t samples);
void ls_audio_stream_end(void);

// Speaker level 0..100. The value is remembered even while the codec is
// released and is applied the next time playback opens the hardware.
void ls_audio_set_volume(uint8_t percent);
uint8_t ls_audio_get_volume(void);
