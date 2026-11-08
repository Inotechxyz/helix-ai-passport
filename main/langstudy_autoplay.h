// main/langstudy_autoplay.h
// Pure auto-play state machine. The UI owns one instance and passes monotonic
// timestamps; this module never calls esp_timer, LVGL or the audio worker, so
// it is exercised by the host tests.
#pragma once

#include "langstudy_model.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    uint32_t lesson_id;  // pinned when auto-play starts
    uint16_t count;      // words found for that lesson
    uint16_t index;      // position inside ids[]
    uint32_t ids[LS_WORDS_MAX];
    int64_t deadline_us;  // when the current word should be replaced
} ls_autoplay_t;

// Stop and clear the loop. Safe on an inactive state.
void ls_autoplay_stop(ls_autoplay_t *ap);

// Loop only the words whose lesson_id matches. Playback starts at start_word_id
// when that word belongs to the lesson, otherwise at the first word. Returns
// false and leaves the state inactive when the lesson has no words or dwell_ms
// is zero.
bool ls_autoplay_start(ls_autoplay_t *ap, const ls_pack_t *pack,
                       uint32_t lesson_id, uint32_t start_word_id,
                       int64_t now_us, uint32_t dwell_ms);

// Stop when active, otherwise start. Returns the new active state.
bool ls_autoplay_toggle(ls_autoplay_t *ap, const ls_pack_t *pack,
                        uint32_t lesson_id, uint32_t start_word_id,
                        int64_t now_us, uint32_t dwell_ms);

// Word currently on screen, or 0 when auto-play is not running.
uint32_t ls_autoplay_current_id(const ls_autoplay_t *ap);

// Advance to the next word of the pinned lesson when the deadline has passed,
// wrapping at the end. Returns true when the displayed word changed. The next
// deadline is measured from now_us so a delayed tick cannot skip words.
bool ls_autoplay_poll(ls_autoplay_t *ap, int64_t now_us, uint32_t dwell_ms);

// Time left on the current word, clamped to [0, dwell_ms].
uint32_t ls_autoplay_remaining_ms(const ls_autoplay_t *ap, int64_t now_us,
                                  uint32_t dwell_ms);

// Remaining fraction in per-mille, for an LVGL bar whose range is 0..1000.
uint16_t ls_autoplay_remaining_permille(const ls_autoplay_t *ap, int64_t now_us,
                                        uint32_t dwell_ms);
