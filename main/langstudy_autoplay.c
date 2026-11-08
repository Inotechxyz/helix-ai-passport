// main/langstudy_autoplay.c
#include "langstudy_autoplay.h"

#include <string.h>

void ls_autoplay_stop(ls_autoplay_t *ap)
{
    if (!ap) return;
    memset(ap, 0, sizeof(*ap));
}

bool ls_autoplay_start(ls_autoplay_t *ap, const ls_pack_t *pack,
                       uint32_t lesson_id, uint32_t start_word_id,
                       int64_t now_us, uint32_t dwell_ms)
{
    if (!ap) return false;
    ls_autoplay_stop(ap);
    if (!pack || dwell_ms == 0) return false;

    uint16_t count = 0;
    uint16_t start = 0;
    for (uint16_t i = 0; i < pack->word_count && i < LS_WORDS_MAX; i++) {
        if (pack->words[i].lesson_id != lesson_id) continue;
        if (pack->words[i].id == start_word_id) start = count;
        ap->ids[count++] = pack->words[i].id;
    }
    if (count == 0) return false;

    ap->active = true;
    ap->lesson_id = lesson_id;
    ap->count = count;
    ap->index = start;
    ap->deadline_us = now_us + (int64_t)dwell_ms * 1000;
    return true;
}

bool ls_autoplay_toggle(ls_autoplay_t *ap, const ls_pack_t *pack,
                        uint32_t lesson_id, uint32_t start_word_id,
                        int64_t now_us, uint32_t dwell_ms)
{
    if (!ap) return false;
    if (ap->active) {
        ls_autoplay_stop(ap);
        return false;
    }
    return ls_autoplay_start(ap, pack, lesson_id, start_word_id, now_us,
                             dwell_ms);
}

uint32_t ls_autoplay_current_id(const ls_autoplay_t *ap)
{
    if (!ap || !ap->active || ap->count == 0 || ap->index >= ap->count) {
        return 0;
    }
    return ap->ids[ap->index];
}

bool ls_autoplay_poll(ls_autoplay_t *ap, int64_t now_us, uint32_t dwell_ms)
{
    if (!ap || !ap->active || ap->count == 0 || dwell_ms == 0) return false;
    if (now_us < ap->deadline_us) return false;

    ap->index = (uint16_t)((ap->index + 1u) % ap->count);
    // Restart from now rather than accumulating the deadline: if the app task
    // was blocked for longer than one dwell, showing every skipped word for
    // 0 ms would be worse than losing the missed ticks.
    ap->deadline_us = now_us + (int64_t)dwell_ms * 1000;
    return true;
}

uint32_t ls_autoplay_remaining_ms(const ls_autoplay_t *ap, int64_t now_us,
                                  uint32_t dwell_ms)
{
    if (!ap || !ap->active || dwell_ms == 0) return 0;
    int64_t remaining = ap->deadline_us - now_us;
    if (remaining <= 0) return 0;
    int64_t total = (int64_t)dwell_ms * 1000;
    if (remaining > total) remaining = total;
    return (uint32_t)((remaining + 999) / 1000);
}

uint16_t ls_autoplay_remaining_permille(const ls_autoplay_t *ap, int64_t now_us,
                                        uint32_t dwell_ms)
{
    if (!ap || !ap->active || dwell_ms == 0) return 0;
    int64_t total = (int64_t)dwell_ms * 1000;
    int64_t remaining = ap->deadline_us - now_us;
    if (remaining <= 0) return 0;
    if (remaining > total) remaining = total;
    // Round up so the bar only empties at the same instant the word changes.
    uint32_t permille = (uint32_t)((remaining * 1000 + total - 1) / total);
    return permille > 1000 ? 1000 : (uint16_t)permille;
}
