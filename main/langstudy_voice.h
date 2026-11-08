// main/langstudy_voice.h
// Pronunciation dispatcher. When Wi-Fi is up it asks the Edge neural voice for
// the word, and silently falls back to the offline formant synthesizer when
// the network, TLS session, token or decoder fails.
//
// ls_voice_speak() never blocks: the request runs on its own worker task, and
// a newer word cancels the one already playing.
#pragma once

#include "esp_err.h"

#include <stdbool.h>

bool ls_voice_speak(const char *text, const char *pron);

// Abandon the word being fetched or spoken and drop any queued one. Navigation
// uses this so a slow online request cannot start talking after the screen has
// already moved on.
void ls_voice_stop(void);

esp_err_t ls_voice_init(void);
void ls_voice_shutdown(void);

// True when a newer word replaced the one currently being spoken.
bool ls_voice_cancelled(void);
