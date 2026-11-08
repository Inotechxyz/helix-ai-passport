// main/langstudy_ui.c
// Notebook-style UI for the language-study application. Everything here runs
// on the single application input task; the LVGL lock is taken by the public
// entry points and released before returning.
#include "langstudy_ui.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "langstudy_audio.h"
#include "langstudy_autoplay.h"
#include "langstudy_buddy.h"
#include "langstudy_buddy_sprite.h"
#include "langstudy_content.h"
#include "langstudy_gen.h"
#include "langstudy_online.h"
#include "langstudy_phoneme.h"
#include "langstudy_progress.h"
#include "langstudy_provision.h"
#include "langstudy_session.h"
#include "langstudy_state.h"
#include "langstudy_status_cache.h"
#include "langstudy_store.h"
#include "langstudy_voice.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Generated 16 px subset covering GB2312 plus ASCII. Words come from the model
// at runtime, so a built-in LVGL CJK font is not enough; this table is the
// character contract for the translation field.
LV_FONT_DECLARE(langstudy_cjk_16);

// ASCII plus the IPA symbols produced by ls_pron_to_ipa(). Kept separate from
// the CJK table so the pronunciation line does not pull in GB2312 glyphs.
LV_FONT_DECLARE(langstudy_ipa_16);

#define LS_IDLE_DIM_MS (30LL * 1000 * 1000)
#define LS_IDLE_SLEEP_MS (300LL * 1000 * 1000)
#define LS_BATTERY_PERIOD_MS (30LL * 1000 * 1000)
// The CW2017 needs a moment after wake before its first conversion; poll it
// quickly until the first valid reading arrives, then fall back to 30 s.
#define LS_BATTERY_FAST_MS (2LL * 1000 * 1000)
#define LS_BATTERY_FAST_TRIES 15

// A warm paper palette with a single red accent, like a passport notebook.
#define LS_UI_PAPER 0xF7F1E3
#define LS_UI_INK 0x26221C
#define LS_UI_MUTED 0x6E6456
#define LS_UI_RULE 0xD9CFBA
#define LS_UI_ACCENT 0x9B3A2E
#define LS_UI_HIGHLIGHT 0xF2C14E
#define LS_UI_OK 0x2E7D32
#define LS_UI_WARN 0xE09A2B

// Vertical battery gauge in the header. The outline is fixed so the slot never
// moves; only the fill's height and colour change.
#define LS_BAT_X 184
#define LS_BAT_Y 8
#define LS_BAT_W 11
#define LS_BAT_H 18
#define LS_BAT_BORDER 2
// One pixel of breathing room inside the border, so a full charge does not look
// like it is bleeding into the outline.
#define LS_BAT_INSET (LS_BAT_BORDER + 1)
#define LS_BAT_FILL_MAX (LS_BAT_H - 2 * LS_BAT_INSET + 1)

typedef enum {
    LS_VIEW_STUDY = 0,
    LS_VIEW_SUMMARY,
    LS_VIEW_MENU,
    LS_VIEW_EXAM,
    LS_VIEW_PROGRESS,
    LS_VIEW_BUDDY,
    LS_VIEW_PROFILE,
    LS_VIEW_SETTINGS,
    LS_VIEW_STATUS,
    LS_VIEW_DETAIL,
    LS_VIEW_PROVISION,
    LS_VIEW_CONFIRM,
} ls_view_t;

typedef enum {
    LS_STATUS_NEXT_STUDY = 0,
    LS_STATUS_NEXT_MENU,
} ls_status_next_t;

typedef enum {
    LS_MENU_REFRESH = 0,
    LS_MENU_PROGRESS,
    LS_MENU_BUDDY,
    LS_MENU_PROFILE,
    LS_MENU_CHECK,
    LS_MENU_SETTINGS,
    LS_MENU_SETUP,
    LS_MENU_NEW_SESSION,
    LS_MENU_RESET,
    LS_MENU_BACK,
    LS_MENU_COUNT,
} ls_menu_item_t;

static const char *const s_menu_text[LS_MENU_COUNT] = {
    "REFRESH WORDS",
    "PROGRESS",
    "BUDDY",
    "CREATE PROFILE",
    "CONNECTION CHECK",
    "SETTINGS",
    "PHONE SETUP",
    "NEW SESSION",
    "RESET PROGRESS",
    "BACK",
};

typedef enum {
    LS_SETTING_LEVEL = 0,
    LS_SETTING_BRIGHTNESS,
    LS_SETTING_VOLUME,
    LS_SETTING_WORD_DELAY,
    LS_SETTING_BACK,
    LS_SETTING_COUNT,
} ls_setting_item_t;

#define LS_EXAM_ROWS (LS_EXAM_COUNT + 1)  // tiers plus BACK

static const char *TAG = "ls_ui";

// Defined below. The session-complete path may start a refresh automatically
// once the learner has met every word in the active pack.
static void start_online(ls_online_op_t op, const char *message);
// The CREATE PROFILE view and the BUDDY view link to each other, so both need
// a forward declaration.
static void show_buddy(const char *note);
static void create_profile(void);

static lv_obj_t *s_scr;
static lv_obj_t *s_study;
static lv_obj_t *s_modal;
static lv_obj_t *s_bat_fill;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_deepseek_icon;

static lv_obj_t *s_progress;
static lv_obj_t *s_bar;
static lv_obj_t *s_auto_bar;
static lv_obj_t *s_help;
static lv_obj_t *s_word;
static lv_obj_t *s_translation;
static lv_obj_t *s_pos;
static lv_obj_t *s_meaning;
static lv_obj_t *s_example;
static lv_obj_t *s_pron;

static lv_obj_t *s_menu_rows[LS_MENU_COUNT];
static lv_obj_t *s_menu_labels[LS_MENU_COUNT];
static int s_menu_index;

static lv_obj_t *s_exam_rows[LS_EXAM_ROWS];
static lv_obj_t *s_exam_labels[LS_EXAM_ROWS];
static int s_exam_index;

static lv_obj_t *s_set_rows[LS_SETTING_COUNT];
static lv_obj_t *s_set_labels[LS_SETTING_COUNT];
static int s_set_index;
static bool s_set_adjusting;

// BUDDY view. The 24x24 palette-index art is painted into a 2x canvas, which
// needs no image decoder and no LVGL heap (LVGL only gets 24 KiB here).
#define LS_BUDDY_SPRITE_ZOOM 2
#define LS_BUDDY_CANVAS_W (LS_BUDDY_SPRITE_W * LS_BUDDY_SPRITE_ZOOM)
#define LS_BUDDY_CANVAS_H (LS_BUDDY_SPRITE_H * LS_BUDDY_SPRITE_ZOOM)
static uint16_t
    s_buddy_canvas_buf[LS_BUDDY_CANVAS_W * LS_BUDDY_CANVAS_H]
        __attribute__((aligned(4)));
static uint8_t s_profile_name_index;

// Growth earned during the current session, reported on the summary screen.
static uint32_t s_session_xp;
static bool s_session_evolved;
static bool s_session_snack;
static lv_obj_t *s_profile_name_label;

#define LS_PROGRESS_ROWS 2
enum { LS_PROGRESS_SYNC = 0, LS_PROGRESS_BACK };
static lv_obj_t *s_progress_rows[LS_PROGRESS_ROWS];
static lv_obj_t *s_progress_labels[LS_PROGRESS_ROWS];
static int s_progress_index;

static bool s_status_busy;
static ls_status_next_t s_status_next;
// Last failure's diagnostic text. It outlives the status screen so the learner
// can open the detail view with DOWN.
static char s_detail[LS_ONLINE_DETAIL_MAX];

static ls_provision_info_t s_prov_info;
static int64_t s_last_input_ms;
static int64_t s_battery_ms;
static bool s_battery_seen;
static uint8_t s_battery_fast_tries;
static bool s_dimmed;
static volatile bool s_sleep_due;
static ls_view_t s_view = LS_VIEW_STUDY;

// Auto-play state. Memory-only: the mode never survives a view change or a
// reboot, matching the guidance that a real key press always hands control
// back to the learner.
static ls_autoplay_t s_auto;
// Set when the loop cursor moves while the LVGL lock is unavailable, so the
// next tick redraws the card even though `advanced` is false by then.
static bool s_auto_redraw;

// ---------------------------------------------------------------------------
// Small widget helpers
// ---------------------------------------------------------------------------

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static lv_obj_t *make_text_label(lv_obj_t *parent, int x, int y, int w, int h,
                                 const lv_font_t *font, uint32_t color,
                                 const char *text)
{
    lv_obj_t *label = make_label(parent, font, color);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

// The header line is narrow, so the charge is a vertical bar filled from the
// bottom rather than a percentage: the title, gauge, Wi-Fi and DeepSeek "D"
// marker share one row. An unreadable gauge leaves the outline empty instead of
// moving the layout or inventing a number.
static void update_battery_bar(int soc)
{
    if (!s_bat_fill) return;
    if (soc < 0) {
        lv_obj_add_flag(s_bat_fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_bat_fill, LV_OBJ_FLAG_HIDDEN);
    if (soc > 100) soc = 100;

    // Round up so any readable charge shows at least one pixel of fill.
    int h = (LS_BAT_FILL_MAX * soc + 99) / 100;
    lv_obj_set_y(s_bat_fill, LS_BAT_Y + LS_BAT_INSET + LS_BAT_FILL_MAX - h);
    lv_obj_set_height(s_bat_fill, h);

    uint32_t color = LS_UI_OK;
    if (soc < 15) color = LS_UI_ACCENT;
    else if (soc < 30) color = LS_UI_WARN;
    lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(color), 0);
}

// The online worker tears the radio down after each operation, so these icons
// reflect the last successful check/refresh rather than a live connection.
// They are hidden until that result is known, exactly as the request asks.
static void update_link_icons(void)
{
    ls_state_t *state = ls_state();
    if (s_wifi_icon) {
        if (state->wifi_ok) {
            lv_obj_remove_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_deepseek_icon) {
        if (state->deepseek_ok) {
            lv_obj_remove_flag(s_deepseek_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_deepseek_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static uint8_t preferred_brightness(void)
{
    uint8_t level = ls_state()->config.brightness;
    return level < LS_BRIGHTNESS_MIN ? LS_BRIGHTNESS_DEFAULT : level;
}

// ---------------------------------------------------------------------------
// Session helpers
// ---------------------------------------------------------------------------

static const ls_word_t *current_word(void)
{
    ls_state_t *state = ls_state();
    if (state->session_len == 0 || state->session_pos >= state->session_len) {
        return NULL;
    }
    return ls_pack_find_word(&state->pack, state->session_ids[state->session_pos]);
}

// Dwell for one auto-play word. A stale or corrupt NVS value is clamped by the
// config loader, but re-check here so the UI never divides by zero.
static uint32_t auto_dwell_ms(void)
{
    ls_state_t *state = ls_state();
    uint8_t seconds = state->config.word_delay_s;
    if (seconds < LS_WORD_DELAY_MIN_S || seconds > LS_WORD_DELAY_MAX_S) {
        seconds = LS_WORD_DELAY_DEFAULT_S;
    }
    return (uint32_t)seconds * 1000u;
}

// In auto-play the visible card is the loop cursor, not the quiz cursor. The
// session cursor stays put so leaving auto-play restores the quiz position.
static const ls_word_t *display_word(void)
{
    if (s_auto.active) {
        const ls_word_t *word =
            ls_pack_find_word(&ls_state()->pack, ls_autoplay_current_id(&s_auto));
        if (word) return word;
    }
    return current_word();
}

static void autoplay_clear(void)
{
    ls_autoplay_stop(&s_auto);
    s_auto_redraw = false;
    if (s_auto_bar) lv_obj_add_flag(s_auto_bar, LV_OBJ_FLAG_HIDDEN);
}

static const ls_lesson_t *lesson_for(const ls_pack_t *pack, uint32_t lesson_id)
{
    for (uint16_t i = 0; i < pack->lesson_count && i < LS_LESSONS_MAX; i++) {
        if (pack->lessons[i].id == lesson_id) return &pack->lessons[i];
    }
    return NULL;
}

static void start_new_session(void)
{
    ls_state_t *state = ls_state();
    autoplay_clear();

    // Keep the session that just ended so the builder can avoid handing the
    // learner the same words twice in a row.
    if (state->session_len > 0) {
        memcpy(state->prev_session_ids, state->session_ids,
               state->session_len * sizeof(state->session_ids[0]));
        state->prev_session_len = state->session_len;
    }

    state->session_len = ls_session_build_ex(
        &state->pack, &state->progress, state->prev_session_ids,
        state->prev_session_len, state->session_ids);
    state->session_pos = 0;
    state->session_correct = 0;

    // Reports for the summary screen; the buddy itself keeps its own XP.
    s_session_xp = 0;
    s_session_evolved = false;
    s_session_snack = false;
}

static void update_study(void)
{
    ls_state_t *state = ls_state();
    const ls_word_t *word = display_word();

    if (!word) {
        lv_label_set_text(s_progress, "NO CARDS");
        lv_label_set_text(s_word, "NO WORDS");
        lv_label_set_text(s_translation, "");
        lv_label_set_text(s_pos, "OPEN THE MENU AND REFRESH");
        lv_label_set_text(s_meaning, "Connect your phone or check the built-in pack.");
        lv_label_set_text(s_example, "");
        lv_label_set_text(s_pron, "");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        if (s_auto_bar) lv_obj_add_flag(s_auto_bar, LV_OBJ_FLAG_HIDDEN);
        if (s_help) lv_label_set_text(s_help, "HOLD OK: MENU");
        return;
    }

    const ls_lesson_t *lesson = lesson_for(&state->pack, word->lesson_id);
    if (s_auto.active) {
        // The strip shows loop position; the dedicated bar below drains over
        // the dwell and is replenished when the word advances.
        lv_label_set_text_fmt(s_progress, "AUTO %u / %u",
                              (unsigned)(s_auto.index + 1),
                              (unsigned)s_auto.count);
        lv_bar_set_range(s_bar, 0, (int32_t)s_auto.count);
        lv_bar_set_value(s_bar, (int32_t)s_auto.index, LV_ANIM_OFF);
        if (s_auto_bar) {
            lv_bar_set_value(s_auto_bar,
                             (int32_t)ls_autoplay_remaining_permille(
                                 &s_auto, esp_timer_get_time(), auto_dwell_ms()),
                             LV_ANIM_OFF);
            lv_obj_remove_flag(s_auto_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_help) lv_label_set_text(s_help, "HOLD DOWN: STOP AUTO");
    } else {
        lv_label_set_text_fmt(s_progress, "CARD %u / %u",
                              (unsigned)(state->session_pos + 1),
                              (unsigned)state->session_len);
        lv_bar_set_range(s_bar, 0, LS_SESSION_CARDS);
        lv_bar_set_value(s_bar, (int32_t)state->session_pos, LV_ANIM_OFF);
        if (s_auto_bar) lv_obj_add_flag(s_auto_bar, LV_OBJ_FLAG_HIDDEN);
        if (s_help) {
            lv_label_set_text(s_help,
                              "UP HEAR   OK GOT IT\nDOWN AGAIN   HOLD OK MENU");
        }
    }
    lv_label_set_text(s_word, word->text);

    if (lesson) {
        lv_label_set_text_fmt(s_pos, "%s  -  %s", word->pos, lesson->title);
    } else {
        lv_label_set_text(s_pos, word->pos);
    }
    lv_label_set_text(s_translation, word->translation);
    lv_label_set_text(s_meaning, word->meaning);
    lv_label_set_text(s_example, word->example);
    if (word->pron[0]) {
        char ipa[LS_IPA_TEXT_MAX];
        if (ls_pron_to_ipa(word->pron, ipa, sizeof(ipa)) > 0) {
            lv_label_set_text(s_pron, ipa);
        } else {
            // Pack validation should reject unknown tokens, but keep a readable
            // fallback rather than leaving the line blank.
            lv_label_set_text_fmt(s_pron, "SAY  %s", word->pron);
        }
    } else {
        lv_label_set_text(s_pron, "");
    }
}

// Leaving auto-play keeps the learner on the word they were just hearing when
// it is part of this quiz session; otherwise the original quiz card returns.
static void autoplay_stop_and_refresh(void)
{
    uint32_t id = ls_autoplay_current_id(&s_auto);
    bool was_active = s_auto.active;
    autoplay_clear();
    if (!was_active) return;

    ls_state_t *state = ls_state();
    for (size_t i = 0; i < state->session_len; i++) {
        if (state->session_ids[i] == id) {
            state->session_pos = i;
            break;
        }
    }
    ls_voice_stop();
    update_study();
}

static void autoplay_toggle(void)
{
    ls_state_t *state = ls_state();
    if (s_auto.active) {
        autoplay_stop_and_refresh();
        return;
    }

    const ls_word_t *word = current_word();
    if (!word) return;
    if (!ls_autoplay_start(&s_auto, &state->pack, word->lesson_id, word->id,
                           esp_timer_get_time(), auto_dwell_ms())) {
        return;
    }
    ls_audio_stop();
    update_study();
    (void)ls_voice_speak(word->text, word->pron);
}

static void speak_current(void)
{
    const ls_word_t *word = display_word();
    if (word) (void)ls_voice_speak(word->text, word->pron);
}

// ---------------------------------------------------------------------------
// View switching
// ---------------------------------------------------------------------------

static lv_obj_t *begin_modal(void)
{
    // Any modal (menu, settings, status) owns input; auto-play must not keep
    // advancing the study card behind it.
    autoplay_clear();
    if (s_modal) {
        lv_obj_delete(s_modal);
        s_modal = NULL;
    }
    if (s_study) lv_obj_add_flag(s_study, LV_OBJ_FLAG_HIDDEN);
    s_modal = make_box(s_scr, 0, 44, 240, 272);
    return s_modal;
}

static void show_study(void)
{
    if (s_modal) {
        lv_obj_delete(s_modal);
        s_modal = NULL;
    }
    if (s_study) lv_obj_remove_flag(s_study, LV_OBJ_FLAG_HIDDEN);
    s_view = LS_VIEW_STUDY;
    update_study();
}

static void show_summary(void)
{
    ls_state_t *state = ls_state();
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_SUMMARY;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "SESSION COMPLETE");
    lv_obj_t *score = make_text_label(m, 10, 40, 220, 40,
                                      &lv_font_montserrat_28, LS_UI_INK, "");
    lv_label_set_text_fmt(score, "%u / %u", (unsigned)state->session_correct,
                          (unsigned)state->session_len);
    make_text_label(m, 10, 88, 220, 20, &lv_font_montserrat_14,
                    LS_UI_MUTED, "CORRECT ANSWERS");

    if (state->buddy_valid) {
        char note[72];
        if (s_session_evolved) {
            snprintf(note, sizeof(note), "%s EVOLVED TO FORM %u!",
                     ls_buddy_name(state->buddy.name_id),
                     (unsigned)(state->buddy.stage + 1));
        } else if (s_session_xp > 0) {
            snprintf(note, sizeof(note), "%s +%lu XP%s",
                     ls_buddy_name(state->buddy.name_id),
                     (unsigned long)s_session_xp,
                     s_session_snack ? "  +1 SNACK" : "");
        } else {
            snprintf(note, sizeof(note), "%s IS WAITING",
                     ls_buddy_name(state->buddy.name_id));
        }
        make_text_label(m, 8, 112, 224, 22, &lv_font_montserrat_14,
                        LS_UI_ACCENT, note);
    }

    make_text_label(m, 10, 140, 220, 60, &lv_font_montserrat_14,
                    LS_UI_INK, "OK: NEXT SESSION\nDOWN: MENU");
}

static void update_menu(void)
{
    ls_state_t *state = ls_state();
    bool available[LS_MENU_COUNT] = {
        [LS_MENU_REFRESH] = ls_config_has_wifi(&state->config) &&
                            ls_config_has_api_key(&state->config),
        // The progress screen works offline; only its sync button needs the
        // network, and it explains the missing setup when pressed.
        [LS_MENU_PROGRESS] = true,
        // The buddy and the profile live entirely on the device; Azure is only
        // involved when the learner syncs progress.
        [LS_MENU_BUDDY] = true,
        [LS_MENU_PROFILE] = true,
        [LS_MENU_CHECK] = true,
        [LS_MENU_SETTINGS] = true,
        [LS_MENU_SETUP] = true,
        [LS_MENU_NEW_SESSION] = true,
        [LS_MENU_RESET] = true,
        [LS_MENU_BACK] = true,
    };

    for (int i = 0; i < LS_MENU_COUNT; i++) {
        if (!s_menu_rows[i] || !s_menu_labels[i]) continue;
        bool selected = (i == s_menu_index);
        lv_obj_set_style_bg_color(s_menu_rows[i],
            lv_color_hex(selected ? LS_UI_HIGHLIGHT : LS_UI_PAPER), 0);
        lv_obj_set_style_border_color(s_menu_rows[i],
            lv_color_hex(selected ? LS_UI_ACCENT : LS_UI_RULE), 0);
        lv_obj_set_style_text_color(s_menu_labels[i],
            lv_color_hex(available[i] ? LS_UI_INK : LS_UI_MUTED), 0);
        if (i == LS_MENU_REFRESH) {
            // Show the selected tier directly so the submenu choice is visible
            // from the main menu.
            lv_label_set_text_fmt(s_menu_labels[i], "REFRESH %s%s",
                                  ls_exam_name(state->config.exam),
                                  available[i] ? "" : "  (SETUP)");
        } else {
            lv_label_set_text_fmt(s_menu_labels[i], "%s%s", s_menu_text[i],
                                  available[i] ? "" : "  (SETUP)");
        }
    }
}

// REFRESH WORDS needs a saved Wi-Fi network and a DeepSeek key. Check them
// before starting the network operation so the learner gets an immediate,
// actionable notice instead of a generic failure after a timeout.
static bool refresh_ready(void)
{
    ls_state_t *state = ls_state();
    return ls_config_has_wifi(&state->config) &&
           ls_config_has_api_key(&state->config);
}

static void show_menu(void)
{
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_MENU;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "MENU");

    // Ten rows: 16 px high on an 18 px pitch keeps the last row above the key
    // hint at y202.
    for (int i = 0; i < LS_MENU_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(m);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, 12, 20 + i * 18);
        lv_obj_set_size(row, 216, 16);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(LS_UI_RULE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(LS_UI_PAPER), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(LS_UI_INK), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(label, s_menu_text[i]);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

        s_menu_rows[i] = row;
        s_menu_labels[i] = label;
    }

    make_text_label(m, 8, 202, 224, 40, &lv_font_montserrat_14,
                    LS_UI_MUTED, "UP/DOWN MOVE\nOK SELECT   HOLD OK BACK");
    update_menu();
}

// ---------------------------------------------------------------------------
// REFRESH WORDS submenu (vocabulary tier)
// ---------------------------------------------------------------------------

static void update_exam(void)
{
    ls_state_t *state = ls_state();
    for (int i = 0; i < LS_EXAM_ROWS; i++) {
        if (!s_exam_rows[i] || !s_exam_labels[i]) continue;
        bool selected = (i == s_exam_index);
        lv_obj_set_style_bg_color(s_exam_rows[i],
            lv_color_hex(selected ? LS_UI_HIGHLIGHT : LS_UI_PAPER), 0);
        lv_obj_set_style_border_color(s_exam_rows[i],
            lv_color_hex(selected ? LS_UI_ACCENT : LS_UI_RULE), 0);
        if (i < LS_EXAM_COUNT) {
            bool current = ((int)state->config.exam == i);
            lv_obj_set_style_text_color(s_exam_labels[i],
                lv_color_hex(current ? LS_UI_ACCENT : LS_UI_INK), 0);
            lv_label_set_text_fmt(s_exam_labels[i], "%s%s",
                                  ls_exam_name((ls_exam_t)i),
                                  current ? "   *" : "");
        } else {
            lv_obj_set_style_text_color(s_exam_labels[i],
                                        lv_color_hex(LS_UI_MUTED), 0);
            lv_label_set_text(s_exam_labels[i], "BACK");
        }
    }
}

static void show_exam(void)
{
    ls_state_t *state = ls_state();
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_EXAM;
    s_exam_index = ((uint8_t)state->config.exam < (uint8_t)LS_EXAM_COUNT)
                       ? (int)state->config.exam
                       : 0;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "REFRESH WORDS");

    for (int i = 0; i < LS_EXAM_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(m);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, 12, 30 + i * 30);
        lv_obj_set_size(row, 216, 26);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(LS_UI_RULE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(LS_UI_PAPER), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(LS_UI_INK), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

        s_exam_rows[i] = row;
        s_exam_labels[i] = label;
    }

    make_text_label(m, 8, 160, 224, 62, &lv_font_montserrat_14,
                    LS_UI_MUTED,
                    "UP/DOWN CHOOSE\nOK REFRESH   HOLD OK BACK\n* = CURRENT TIER");
    update_exam();
}

// ---------------------------------------------------------------------------
// SETTINGS (brightness and volume)
// ---------------------------------------------------------------------------

static lv_obj_t *s_set_hint;

static void update_settings(void)
{
    ls_config_t *config = &ls_state()->config;
    for (int i = 0; i < LS_SETTING_COUNT; i++) {
        if (!s_set_rows[i] || !s_set_labels[i]) continue;
        bool selected = (i == s_set_index);
        lv_obj_set_style_bg_color(s_set_rows[i],
            lv_color_hex(selected ? LS_UI_HIGHLIGHT : LS_UI_PAPER), 0);
        lv_obj_set_style_border_color(s_set_rows[i],
            lv_color_hex(selected ? LS_UI_ACCENT : LS_UI_RULE), 0);
        lv_obj_set_style_text_color(s_set_labels[i], lv_color_hex(LS_UI_INK), 0);
        switch ((ls_setting_item_t)i) {
        case LS_SETTING_LEVEL:
            lv_label_set_text_fmt(s_set_labels[i], "ENGLISH LEVEL %u/10",
                                  (unsigned)config->level);
            break;
        case LS_SETTING_BRIGHTNESS:
            lv_label_set_text_fmt(s_set_labels[i], "BRIGHTNESS   %u%%",
                                  (unsigned)config->brightness);
            break;
        case LS_SETTING_VOLUME:
            lv_label_set_text_fmt(s_set_labels[i], "VOLUME       %u%%",
                                  (unsigned)config->volume);
            break;
        case LS_SETTING_WORD_DELAY:
            lv_label_set_text_fmt(s_set_labels[i], "WORD DELAY   %us",
                                  (unsigned)config->word_delay_s);
            break;
        case LS_SETTING_BACK:
        default:
            lv_label_set_text(s_set_labels[i], "BACK");
            break;
        }
    }
    if (s_set_hint) {
        lv_label_set_text(s_set_hint,
                          s_set_adjusting
                              ? "UP/DOWN ADJUST\nOK DONE"
                              : "UP/DOWN MOVE\nOK ADJUST   HOLD OK BACK");
    }
}

static void show_settings(void)
{
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_SETTINGS;
    s_set_index = LS_SETTING_BRIGHTNESS;
    s_set_adjusting = false;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "SETTINGS");

    for (int i = 0; i < LS_SETTING_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(m);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, 12, 30 + i * 28);
        lv_obj_set_size(row, 216, 24);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(LS_UI_RULE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(LS_UI_PAPER), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(LS_UI_INK), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

        s_set_rows[i] = row;
        s_set_labels[i] = label;
    }

    s_set_hint = make_text_label(m, 8, 178, 224, 60, &lv_font_montserrat_14,
                                 LS_UI_MUTED, "");
    update_settings();
}

static void adjust_setting(int steps)
{
    ls_state_t *state = ls_state();
    int delta = steps * LS_LEVEL_STEP;

    if (s_set_index == LS_SETTING_LEVEL) {
        // One level per step, 1 = starter .. 10 = professional.
        int value = (int)state->config.level + steps;
        if (value < LS_ENGLISH_LEVEL_MIN) value = LS_ENGLISH_LEVEL_MIN;
        if (value > LS_ENGLISH_LEVEL_MAX) value = LS_ENGLISH_LEVEL_MAX;
        state->config.level = (uint8_t)value;
    } else if (s_set_index == LS_SETTING_BRIGHTNESS) {
        int value = (int)state->config.brightness + delta;
        if (value < LS_BRIGHTNESS_MIN) value = LS_BRIGHTNESS_MIN;
        if (value > 100) value = 100;
        state->config.brightness = (uint8_t)value;
        bsp_display_backlight(state->config.brightness);
    } else if (s_set_index == LS_SETTING_VOLUME) {
        int value = (int)state->config.volume + delta;
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        state->config.volume = (uint8_t)value;
        ls_audio_set_volume(state->config.volume);
        // Speak a full word so the new level is obvious, not just a short vowel.
        (void)ls_audio_speak("HH AH L OW");
    } else if (s_set_index == LS_SETTING_WORD_DELAY) {
        // One second per step, unlike the 10% level controls above.
        int value = (int)state->config.word_delay_s + steps;
        if (value < LS_WORD_DELAY_MIN_S) value = LS_WORD_DELAY_MIN_S;
        if (value > LS_WORD_DELAY_MAX_S) value = LS_WORD_DELAY_MAX_S;
        state->config.word_delay_s = (uint8_t)value;
    } else {
        return;
    }

    if (state->storage_ready) {
        (void)ls_config_save_preferences(state->config.level,
                                         state->config.brightness,
                                         state->config.volume,
                                         state->config.word_delay_s);
    }
    update_settings();
}

static void show_status_ex(const char *title, const char *message, bool busy,
                           ls_status_next_t next, const char *detail)
{
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_STATUS;
    s_status_busy = busy;
    s_status_next = next;
    snprintf(s_detail, sizeof(s_detail), "%s", detail ? detail : "");

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, title ? title : (busy ? "WORKING" : "ONLINE"));
    make_text_label(m, 10, 34, 220, 130, &lv_font_montserrat_14,
                    LS_UI_INK, message);
    make_text_label(m, 10, 188, 220, 44, &lv_font_montserrat_14,
                    LS_UI_MUTED,
                    busy ? "PLEASE WAIT..."
                         : (s_detail[0] ? "OK: CONTINUE\nDOWN: DETAIL"
                                        : "OK: CONTINUE"));
}

static void show_status_titled(const char *title, const char *message,
                               bool busy, ls_status_next_t next)
{
    show_status_ex(title, message, busy, next, NULL);
}

static void show_detail_view(void)
{
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_DETAIL;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "ERROR DETAIL");
    make_text_label(m, 10, 26, 220, 205, &lv_font_montserrat_14,
                    LS_UI_INK, s_detail);
    make_text_label(m, 10, 236, 220, 30, &lv_font_montserrat_14,
                    LS_UI_MUTED, "OK: BACK");
}

static void show_refresh_not_ready(void)
{
    show_status_titled("REFRESH WORDS",
                       "WI-FI OR DEEPSEEK\nNOT SET UP\n\n"
                       "OPEN PHONE SETUP,\nADD BOTH, THEN RETRY",
                       false, LS_STATUS_NEXT_MENU);
}

static void show_status(const char *message, bool busy, ls_status_next_t next)
{
    show_status_titled(NULL, message, busy, next);
}

static void show_provision(void)
{
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_PROVISION;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "PHONE SETUP");
    make_text_label(m, 10, 28, 220, 18, &lv_font_montserrat_14,
                    LS_UI_MUTED, "1. JOIN THIS WI-FI");
    make_text_label(m, 10, 46, 220, 28, &lv_font_montserrat_20,
                    LS_UI_INK, s_prov_info.ap_ssid);
    make_text_label(m, 10, 78, 220, 18, &lv_font_montserrat_14,
                    LS_UI_MUTED, "PASSWORD");
    make_text_label(m, 10, 96, 220, 28, &lv_font_montserrat_20,
                    LS_UI_INK, s_prov_info.ap_password);
    make_text_label(m, 10, 130, 220, 18, &lv_font_montserrat_14,
                    LS_UI_MUTED, "2. OPEN IN A BROWSER");
    make_text_label(m, 10, 148, 220, 28, &lv_font_montserrat_20,
                    LS_UI_INK, s_prov_info.url);
    make_text_label(m, 8, 196, 224, 40, &lv_font_montserrat_14,
                    LS_UI_INK, "SET WI-FI + DEEPSEEK KEY\nHOLD OK TO FINISH");
}

static void show_confirm(void)
{
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_CONFIRM;

    make_text_label(m, 10, 10, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "RESET PROGRESS?");
    make_text_label(m, 12, 50, 216, 50, &lv_font_montserrat_14,
                    LS_UI_INK,
                    "ALL LEARNING RECORDS AND THE\nBUDDY PROFILE WILL BE ERASED.");
    make_text_label(m, 10, 130, 220, 60, &lv_font_montserrat_14,
                    LS_UI_INK, "OK: YES\nDOWN: NO");
}

// ---------------------------------------------------------------------------
// PROGRESS (statistics plus the optional sync button)
// ---------------------------------------------------------------------------

static bool sync_ready(void)
{
    ls_state_t *state = ls_state();
    return ls_config_has_wifi(&state->config) &&
           ls_config_has_sync_url(&state->config) &&
           state->profile_id[0] != '\0';
}

static unsigned studied_word_count(void)
{
    ls_state_t *state = ls_state();
    unsigned count = 0;
    for (uint16_t i = 0;
         i < state->progress.record_count && i < LS_PROGRESS_RECORDS_MAX; i++) {
        if (ls_pack_find_word(&state->pack, state->progress.records[i].word_id)) {
            count++;
        }
    }
    return count;
}

static void update_progress_view(void)
{
    bool can_sync = sync_ready();
    for (int i = 0; i < LS_PROGRESS_ROWS; i++) {
        if (!s_progress_rows[i] || !s_progress_labels[i]) continue;
        bool selected = (i == s_progress_index);
        lv_obj_set_style_bg_color(s_progress_rows[i],
            lv_color_hex(selected ? LS_UI_HIGHLIGHT : LS_UI_PAPER), 0);
        lv_obj_set_style_border_color(s_progress_rows[i],
            lv_color_hex(selected ? LS_UI_ACCENT : LS_UI_RULE), 0);
        if (i == LS_PROGRESS_SYNC) {
            lv_obj_set_style_text_color(s_progress_labels[i],
                lv_color_hex(can_sync ? LS_UI_INK : LS_UI_MUTED), 0);
            lv_label_set_text_fmt(s_progress_labels[i], "SYNC NOW%s",
                                  can_sync ? "" : "  (SETUP)");
        } else {
            lv_obj_set_style_text_color(s_progress_labels[i],
                                        lv_color_hex(LS_UI_INK), 0);
            lv_label_set_text(s_progress_labels[i], "BACK");
        }
    }
}

static void show_progress(void)
{
    ls_state_t *state = ls_state();
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_PROGRESS;
    s_progress_index = 0;

    unsigned studied = studied_word_count();
    unsigned total = state->pack.word_count;
    unsigned completion = total ? (studied * 100u + total / 2u) / total : 0;
    unsigned accuracy = state->progress.answered_total
                            ? (state->progress.correct_total * 100u +
                               state->progress.answered_total / 2u) /
                                  state->progress.answered_total
                            : 0;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "PROGRESS");

    // Two columns keep five statistics readable without a scrollable list.
    static const char *const names[] = {"SESSIONS", "ANSWERED", "ACCURACY",
                                        "STUDIED", "COMPLETION"};
    char values[5][24];
    snprintf(values[0], sizeof(values[0]), "%u", (unsigned)state->progress.session_count);
    snprintf(values[1], sizeof(values[1]), "%u", (unsigned)state->progress.answered_total);
    snprintf(values[2], sizeof(values[2]), "%u%%", accuracy);
    snprintf(values[3], sizeof(values[3]), "%u / %u", studied, total);
    snprintf(values[4], sizeof(values[4]), "%u%%", completion);
    for (int i = 0; i < 5; i++) {
        make_text_label(m, 12, 28 + i * 22, 120, 20, &lv_font_montserrat_14,
                        LS_UI_MUTED, names[i]);
        lv_obj_t *value = make_text_label(m, 130, 28 + i * 22, 96, 20,
                                          &lv_font_montserrat_14, LS_UI_INK, "");
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(value, values[i]);
    }

    for (int i = 0; i < LS_PROGRESS_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(m);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(row, 12, 144 + i * 30);
        lv_obj_set_size(row, 216, 26);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(LS_UI_RULE), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(LS_UI_PAPER), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(LS_UI_INK), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

        s_progress_rows[i] = row;
        s_progress_labels[i] = label;
    }

    make_text_label(m, 8, 212, 224, 50, &lv_font_montserrat_14,
                    LS_UI_MUTED, "UP/DOWN MOVE\nOK SELECT   HOLD OK BACK");
    update_progress_view();
}

// ---------------------------------------------------------------------------
// Digital buddy
// ---------------------------------------------------------------------------

static void update_profile_name(void)
{
    if (!s_profile_name_label) return;
    s_profile_name_index %= LS_BUDDY_NAME_COUNT;
    lv_label_set_text(s_profile_name_label,
                      ls_buddy_name(s_profile_name_index));
}

static void show_profile(void)
{
    ls_state_t *state = ls_state();
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_PROFILE;
    s_profile_name_label = NULL;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "CREATE PROFILE");

    if (state->profile_id[0] != '\0') {
        // The id names the Azure folder. Replacing it silently would orphan
        // the remote copy, so this screen only reports the existing one.
        make_text_label(m, 10, 32, 220, 20, &lv_font_montserrat_14,
                        LS_UI_MUTED, "THIS DEVICE ALREADY HAS ONE");

        char first[19];
        char second[19];
        memcpy(first, state->profile_id, 18);
        first[18] = '\0';
        memcpy(second, state->profile_id + 18, 18);
        second[18] = '\0';
        make_text_label(m, 10, 62, 220, 20, &lv_font_montserrat_14,
                        LS_UI_INK, first);
        make_text_label(m, 10, 84, 220, 20, &lv_font_montserrat_14,
                        LS_UI_INK, second);
        make_text_label(m, 10, 122, 220, 60, &lv_font_montserrat_14,
                        LS_UI_MUTED,
                        "RESET PROGRESS REMOVES THE\nPROFILE AND ITS BUDDY.");
        make_text_label(m, 8, 212, 224, 40, &lv_font_montserrat_14,
                        LS_UI_MUTED, "OK: BACK");
        return;
    }

    make_text_label(m, 10, 28, 220, 20, &lv_font_montserrat_14,
                    LS_UI_MUTED, "PICK A NAME FOR YOUR BUDDY");
    s_profile_name_label = make_text_label(m, 10, 58, 220, 34,
                                           &lv_font_montserrat_28,
                                           LS_UI_INK, "");
    update_profile_name();
    make_text_label(m, 10, 106, 220, 60, &lv_font_montserrat_14,
                    LS_UI_MUTED,
                    "OK CREATES A UNIQUE PROFILE ID\nAND A RANDOM BUDDY.");
    make_text_label(m, 8, 212, 224, 50, &lv_font_montserrat_14,
                    LS_UI_MUTED, "UP/DOWN NAME\nOK CREATE   HOLD OK BACK");
}

// Paint the 24x24 palette-index art into the 2x canvas. Index 0 is
// transparent, so the paper background shows through.
static void paint_buddy(lv_obj_t *parent, const ls_buddy_t *buddy)
{
    const uint8_t *px = ls_buddy_sprite(buddy->species, buddy->stage);
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, s_buddy_canvas_buf, LS_BUDDY_CANVAS_W,
                         LS_BUDDY_CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(canvas, (240 - LS_BUDDY_CANVAS_W) / 2, 14);
    lv_obj_set_style_pad_all(canvas, 0, 0);
    lv_obj_set_style_border_width(canvas, 0, 0);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);

    for (int y = 0; y < LS_BUDDY_SPRITE_H; y++) {
        for (int x = 0; x < LS_BUDDY_SPRITE_W; x++) {
            uint8_t index = px[y * LS_BUDDY_SPRITE_W + x];
            lv_color_t color =
                index ? lv_color_hex(ls_buddy_palette_color(buddy->species,
                                                            index))
                      : lv_color_black();
            lv_opa_t opa = index ? LV_OPA_COVER : LV_OPA_TRANSP;
            for (int dy = 0; dy < LS_BUDDY_SPRITE_ZOOM; dy++) {
                for (int dx = 0; dx < LS_BUDDY_SPRITE_ZOOM; dx++) {
                    lv_canvas_set_px(canvas,
                                     x * LS_BUDDY_SPRITE_ZOOM + dx,
                                     y * LS_BUDDY_SPRITE_ZOOM + dy,
                                     color, opa);
                }
            }
        }
    }
}

static void buddy_bar(lv_obj_t *parent, int y, int permille, uint32_t color)
{
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    lv_obj_t *track = make_box(parent, 20, y, 200, 8);
    lv_obj_set_style_radius(track, 4, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(LS_UI_RULE), 0);
    lv_obj_t *fill = make_box(parent, 20, y, 200 * permille / 1000, 8);
    lv_obj_set_style_radius(fill, 4, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
}

static void show_buddy(const char *note)
{
    ls_state_t *state = ls_state();
    lv_obj_t *m = begin_modal();
    s_view = LS_VIEW_BUDDY;

    make_text_label(m, 10, 0, 220, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, "BUDDY");

    if (!state->buddy_valid) {
        make_text_label(m, 12, 90, 216, 100, &lv_font_montserrat_14,
                        LS_UI_MUTED,
                        "NO BUDDY YET\n\nCREATE A PROFILE FROM\nTHE MENU");
        make_text_label(m, 8, 212, 224, 40, &lv_font_montserrat_14,
                        LS_UI_MUTED, "OK: BACK");
        return;
    }

    ls_buddy_t *buddy = &state->buddy;
    paint_buddy(m, buddy);

    char line[64];
    snprintf(line, sizeof(line), "%s THE %s",
             ls_buddy_name(buddy->name_id),
             ls_buddy_species_name(buddy->species));
    make_text_label(m, 8, 68, 224, 20, &lv_font_montserrat_14,
                    LS_UI_INK, line);

    uint32_t next = ls_buddy_next_stage_xp(buddy->stage);
    if (next == 0) {
        snprintf(line, sizeof(line), "FORM %u/4   XP %lu  MAX",
                 (unsigned)buddy->stage + 1, (unsigned long)buddy->xp);
    } else {
        snprintf(line, sizeof(line), "FORM %u/4   XP %lu / %lu",
                 (unsigned)buddy->stage + 1, (unsigned long)buddy->xp,
                 (unsigned long)next);
    }
    make_text_label(m, 8, 90, 224, 20, &lv_font_montserrat_14,
                    LS_UI_MUTED, line);

    // XP bar toward the next evolution, relative to this stage's floor so it
    // starts empty after each growth.
    uint32_t floor = 0;
    if (buddy->stage == 1) floor = LS_BUDDY_XP_STAGE1;
    else if (buddy->stage == 2) floor = LS_BUDDY_XP_STAGE2;
    int xp_permille = 1000;
    if (next > floor) {
        xp_permille = (int)((buddy->xp - floor) * 1000u / (next - floor));
    }
    buddy_bar(m, 112, xp_permille, LS_UI_ACCENT);

    snprintf(line, sizeof(line), "ENERGY %u%%   SNACKS %u",
             (unsigned)buddy->energy, (unsigned)buddy->snacks);
    make_text_label(m, 8, 126, 224, 20, &lv_font_montserrat_14,
                    LS_UI_MUTED, line);
    buddy_bar(m, 148, (int)buddy->energy * 10,
              ls_buddy_is_happy(buddy) ? LS_UI_OK : LS_UI_WARN);

    snprintf(line, sizeof(line), "SKILL: %s",
             buddy->stage == 0 ? "NONE"
                               : ls_buddy_skill_name(buddy->stage));
    make_text_label(m, 8, 162, 224, 20, &lv_font_montserrat_14,
                    LS_UI_INK, line);
    make_text_label(m, 8, 180, 224, 20, &lv_font_montserrat_14,
                    LS_UI_MUTED, ls_buddy_skill_detail(buddy->stage));

    if (note && note[0]) {
        snprintf(line, sizeof(line), "%s", note);
    } else if (ls_buddy_is_happy(buddy)) {
        snprintf(line, sizeof(line), "HAPPY: +25%% XP");
    } else {
        snprintf(line, sizeof(line), "FEED TO GET HAPPY");
    }
    make_text_label(m, 8, 200, 224, 20, &lv_font_montserrat_14,
                    LS_UI_ACCENT, line);

    make_text_label(m, 8, 224, 224, 40, &lv_font_montserrat_14,
                    LS_UI_MUTED, "OK FEED   HOLD OK BACK");
}

static void create_profile(void)
{
    ls_state_t *state = ls_state();

    uint32_t bits[4] = {esp_random(), esp_random(), esp_random(),
                        esp_random()};
    char id[LS_PROFILE_ID_MAX + 1];
    ls_profile_format(bits, id);
    if (!ls_profile_valid(id)) {
        show_status("COULD NOT CREATE\nA PROFILE ID", false,
                    LS_STATUS_NEXT_MENU);
        return;
    }

    memcpy(state->profile_id, id, sizeof(state->profile_id));
    s_profile_name_index %= LS_BUDDY_NAME_COUNT;
    ls_buddy_init(&state->buddy,
                  (uint8_t)(esp_random() % LS_BUDDY_SPECIES_COUNT),
                  s_profile_name_index);
    state->buddy_valid = true;

    bool saved = false;
    if (state->storage_ready) {
        saved = ls_store_save_profile_id(id) == ESP_OK &&
                ls_store_save_buddy(&state->buddy) == ESP_OK;
    }
    ESP_LOGI(TAG, "profile %s created (saved=%d)", id, (int)saved);
    show_buddy(saved ? "PROFILE CREATED"
                     : "PROFILE CREATED\nNOT SAVED");
}

static void feed_buddy(void)
{
    ls_state_t *state = ls_state();
    if (!state->buddy_valid) {
        show_menu();
        return;
    }

    ls_buddy_feed_t result = ls_buddy_feed(&state->buddy);
    if (result == LS_BUDDY_FED && state->storage_ready) {
        (void)ls_store_save_buddy(&state->buddy);
    }

    switch (result) {
    case LS_BUDDY_FED:
        show_buddy("FED! +25 ENERGY");
        break;
    case LS_BUDDY_ALREADY_FULL:
        show_buddy("ALREADY FULL");
        break;
    case LS_BUDDY_NO_SNACK:
    default:
        show_buddy("NO SNACKS LEFT");
        break;
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

// Completion is judged against the active pack: every word needs a progress
// record. Once the learner has met them all, a configured connection pulls
// fresh vocabulary automatically instead of cycling the same pack forever.
// "Ready" means stored Wi-Fi plus a DeepSeek key; an unreachable network still
// fails visibly rather than being silently skipped.
static void maybe_auto_refresh(void)
{
    ls_state_t *state = ls_state();
    if (!refresh_ready() || ls_online_busy()) return;
    if (state->pack.word_count == 0) return;
    if (studied_word_count() < state->pack.word_count) return;

    char message[64];
    snprintf(message, sizeof(message), "ALL WORDS STUDIED\nREFRESHING %s",
             ls_exam_name(state->config.exam));
    start_online(LS_ONLINE_REFRESH, message);
}

static void answer_card(bool correct)
{
    ls_state_t *state = ls_state();
    const ls_word_t *word = current_word();
    if (!word) {
        show_summary();
        return;
    }

    if (correct) state->session_correct++;
    ls_progress_answer(&state->progress, word->id, correct);
    if (state->storage_ready) {
        (void)ls_store_save_progress(&state->progress);
    }

    // The buddy earns XP from the same answer. Higher English levels are worth
    // more, and the per-answer energy drift keeps condition tied to activity.
    if (state->buddy_valid) {
        bool evolved = false;
        s_session_xp += ls_buddy_award(&state->buddy, state->config.level,
                                       correct, &evolved);
        if (evolved) s_session_evolved = true;
        if (state->storage_ready) {
            (void)ls_store_save_buddy(&state->buddy);
        }
    }

    state->session_pos++;
    ls_voice_stop();
    if (state->session_pos >= state->session_len) {
        ls_progress_finish_session(&state->progress);
        if (state->storage_ready) {
            (void)ls_store_save_progress(&state->progress);
        }

        // Ending a session drains condition and may earn a snack for accuracy.
        if (state->buddy_valid) {
            bool evolved = false;
            bool snack = false;
            ls_buddy_finish_session(&state->buddy, state->session_correct,
                                    (uint16_t)state->session_len, &snack,
                                    &evolved);
            if (evolved) s_session_evolved = true;
            if (snack) s_session_snack = true;
            if (state->storage_ready) {
                (void)ls_store_save_buddy(&state->buddy);
            }
        }
        show_summary();
        // 100% completion is the natural moment to replace the pack: the
        // learner has seen every word, and the effective level derived from
        // the growing session count makes the next set a step harder.
        maybe_auto_refresh();
    } else {
        update_study();
    }

    // ECHO (second evolution): repeat a missed word so the learner hears it
    // again at the moment of the mistake.
    if (!correct && state->buddy_valid && state->buddy.stage >= 2) {
        (void)ls_voice_speak(word->text, word->pron);
    }
}

static void apply_refresh(ls_gen_pack_t *pack, const char *note)
{
    ls_state_t *state = ls_state();

    // Match records by text first: the generated pack has different IDs.
    ls_progress_remap_words(&state->progress, state->pack.words,
                            state->pack.word_count, pack->words,
                            pack->word_count);
    ls_progress_drop_missing_words(&state->progress, pack->words,
                                   pack->word_count);

    // Promote the staging model into the active pack before saving; storage and
    // the rest of the UI operate on the full ls_pack_t.
    ls_pack_copy_generated(&state->pack, pack);

    uint32_t generation = 0;
    esp_err_t saved = ESP_FAIL;
    if (state->storage_ready) {
        saved = ls_store_save_pack_model(&state->pack, &generation);
    }

    state->pack_crc = ls_pack_crc(&state->pack);
    if (saved == ESP_OK) {
        state->store.from_storage = true;
        state->store.generation = generation;
    }
    // Remember the accepted vocabulary so the next REFRESH excludes it even
    // after the active pack is replaced.
    ls_gen_append_history(state->avoid_history, sizeof(state->avoid_history),
                          &state->pack);
    if (state->storage_ready) {
        (void)ls_store_save_avoid_history(state->avoid_history);
        (void)ls_store_save_progress(&state->progress);
    }

    // The new pack has different IDs/words; the old exclusion list is
    // meaningless and must not shrink the first session. Clear session_len as
    // well, because start_new_session() would otherwise promote the old
    // session's IDs back into the exclusion list.
    state->session_len = 0;
    state->prev_session_len = 0;
    start_new_session();

    const char *updated = (!state->storage_ready || saved != ESP_OK)
                              ? "WORDS UPDATED IN RAM\nSTORAGE DID NOT SAVE"
                              : "WORDS UPDATED";
    if (note && note[0]) {
        // Repeats no longer reject a pack. Keep the notice visible on the
        // status screen and leave the full word list behind DOWN: DETAIL.
        char message[96];
        snprintf(message, sizeof(message), "%s\nSOME WORDS REPEATED", updated);
        show_status_ex("WORDS UPDATED", message, false, LS_STATUS_NEXT_STUDY,
                       note);
    } else {
        show_status(updated, false, LS_STATUS_NEXT_STUDY);
    }
}

static void apply_sync(ls_progress_t *remote)
{
    ls_state_t *state = ls_state();
    ls_progress_merge(&state->progress, remote);
    ls_progress_drop_missing(&state->progress, &state->pack);
    if (state->storage_ready) {
        (void)ls_store_save_progress(&state->progress);
    }
    start_new_session();
    show_status("PROGRESS SYNCED", false, LS_STATUS_NEXT_STUDY);
}

static void handle_online(ls_online_msg_t *msg)
{
    ls_state_t *state = ls_state();
    if (msg->link_flags & LS_LINK_WIFI_KNOWN) {
        state->wifi_ok = (msg->link_flags & LS_LINK_WIFI_OK) != 0;
    }
    if (msg->link_flags & LS_LINK_DEEPSEEK_KNOWN) {
        state->deepseek_ok = (msg->link_flags & LS_LINK_DEEPSEEK_OK) != 0;
    }
    // Deep sleep resets the flags; save them so the next wake can draw the
    // header before any online operation runs.
    ls_status_cache_set_links(state->wifi_ok, state->deepseek_ok);
    update_link_icons();

    switch (msg->kind) {
    case LS_ONLINE_REFRESH_OK:
        if (msg->pack) {
            apply_refresh(msg->pack, msg->detail);
        } else {
            show_status("REFRESH FAILED", false, LS_STATUS_NEXT_MENU);
        }
        break;
    case LS_ONLINE_REFRESH_FAIL:
        show_status_ex("REFRESH WORDS",
                       msg->message[0] ? msg->message : "REFRESH FAILED", false,
                       LS_STATUS_NEXT_MENU, msg->detail);
        break;
    case LS_ONLINE_SYNC_OK:
        if (msg->progress) {
            apply_sync(msg->progress);
        } else {
            show_status("SYNC FAILED", false, LS_STATUS_NEXT_MENU);
        }
        break;
    case LS_ONLINE_SYNC_FAIL:
        show_status_ex("SYNC PROGRESS",
                       msg->message[0] ? msg->message : "SYNC FAILED", false,
                       LS_STATUS_NEXT_MENU, msg->detail);
        break;
    case LS_ONLINE_CHECK_OK:
        show_status_titled("CONNECTION OK", msg->message, false,
                           LS_STATUS_NEXT_MENU);
        break;
    case LS_ONLINE_CHECK_FAIL:
        show_status_titled("CONNECTION", msg->message, false,
                           LS_STATUS_NEXT_MENU);
        break;
    }
    free(msg->pack);
    free(msg->progress);
    msg->pack = NULL;
    msg->progress = NULL;
}

static void start_online(ls_online_op_t op, const char *message)
{
    if (ls_provision_active()) {
        ls_provision_stop();
        ls_audio_resume();
    }
    if (!ls_online_start(op)) {
        show_status("COULD NOT START\nTRY AGAIN", false, LS_STATUS_NEXT_MENU);
        return;
    }
    show_status(message, true, LS_STATUS_NEXT_MENU);
}

static void activate_progress(void)
{
    if (s_progress_index == LS_PROGRESS_BACK) {
        show_menu();
        return;
    }
    if (!sync_ready()) {
        show_status_titled("SYNC PROGRESS",
                           "SYNC NOT SET UP\n\n"
                           "ADD WI-FI + AZURE SAS URL\n"
                           "IN PHONE SETUP, THEN\n"
                           "CREATE A PROFILE",
                           false, LS_STATUS_NEXT_MENU);
        return;
    }
    start_online(LS_ONLINE_SYNC, "SYNCING PROGRESS");
}

static void begin_provisioning(void)
{
    ls_state_t *state = ls_state();
    if (ls_online_busy()) return;

    // SoftAP + HTTP server need internal RAM; return the I2S/codec buffers
    // first and re-enable playback when provisioning stops.
    ls_voice_stop();
    (void)ls_audio_release();

    esp_err_t err = ls_provision_start(&state->config, &s_prov_info);
    if (err != ESP_OK) {
        ls_audio_resume();
        ESP_LOGW(TAG, "provisioning start failed: %s", esp_err_to_name(err));
        show_status("COULD NOT START\nPHONE SETUP", false, LS_STATUS_NEXT_MENU);
        return;
    }
    show_provision();
}

static void do_reset(void)
{
    ls_state_t *state = ls_state();
    ls_progress_init(&state->progress);
    // RESET means a fresh start, including vocabulary that earlier refreshes
    // pushed away and the profile/buddy identity.
    state->avoid_history[0] = '\0';
    state->profile_id[0] = '\0';
    state->buddy_valid = false;
    memset(&state->buddy, 0, sizeof(state->buddy));
    if (state->storage_ready) {
        (void)ls_store_clear_progress();
        (void)ls_store_clear_avoid_history();
        // This also erases the buddy record that belongs to the profile.
        (void)ls_store_clear_profile_id();
    }
    // A fresh start may reuse any word; dropping session_len also stops
    // start_new_session() from carrying the old exclusion list forward.
    state->session_len = 0;
    state->prev_session_len = 0;
    start_new_session();
    show_study();
}

static void start_refresh_for_exam(ls_exam_t exam)
{
    ls_state_t *state = ls_state();
    if (!refresh_ready()) {
        show_refresh_not_ready();
        return;
    }
    state->config.exam = exam;
    if (state->storage_ready) {
        (void)ls_config_save_exam(exam);
    }
    char message[64];
    snprintf(message, sizeof(message), "REFRESHING %s WORDS\nUSING DEEPSEEK",
             ls_exam_name(exam));
    start_online(LS_ONLINE_REFRESH, message);
}

static void choose_exam(void)
{
    if (s_exam_index >= LS_EXAM_COUNT) {
        show_menu();
        return;
    }
    start_refresh_for_exam((ls_exam_t)s_exam_index);
}

static void activate_menu(void)
{
    switch ((ls_menu_item_t)s_menu_index) {
    case LS_MENU_REFRESH:
        // Warn before showing the tier picker; choosing a tier would otherwise
        // start a network attempt that cannot succeed.
        if (!refresh_ready()) {
            show_refresh_not_ready();
            break;
        }
        show_exam();
        break;
    case LS_MENU_PROGRESS:
        show_progress();
        break;
    case LS_MENU_BUDDY:
        show_buddy(NULL);
        break;
    case LS_MENU_PROFILE:
        show_profile();
        break;
    case LS_MENU_CHECK:
        start_online(LS_ONLINE_CHECK, "WI-FI / DEEPSEEK\nPLEASE WAIT");
        break;
    case LS_MENU_SETTINGS:
        show_settings();
        break;
    case LS_MENU_SETUP:
        begin_provisioning();
        break;
    case LS_MENU_NEW_SESSION:
        start_new_session();
        show_study();
        break;
    case LS_MENU_RESET:
        show_confirm();
        break;
    case LS_MENU_BACK:
    default:
        show_study();
        break;
    }
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

esp_err_t ls_ui_init(void)
{
    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;

    start_new_session();

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(LS_UI_PAPER), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    // Title, battery gauge, Wi-Fi and DeepSeek status share one line.
    // Widths measured with lv_font_montserrat_14: the title is 146 px, so the
    // gauge sits at 184 and leaves the link icons at 197 and 215 untouched.
    lv_obj_t *title = make_label(s_scr, &lv_font_montserrat_14, LS_UI_INK);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(title, 12, 8);
    lv_obj_set_size(title, 148, 18);
    lv_label_set_text(title, "PASSPORT ENGLISH");

    // Battery: a 1 px nub and an outlined slot, with the charge drawn as a
    // vertical fill from the bottom. Green is normal, amber below 30%, red
    // below 15%.
    lv_obj_t *bat_nub = make_box(s_scr, LS_BAT_X + (LS_BAT_W - 5) / 2,
                                 LS_BAT_Y - 2, 5, 2);
    lv_obj_set_style_bg_opa(bat_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bat_nub, lv_color_hex(LS_UI_MUTED), 0);
    lv_obj_t *bat_shell = make_box(s_scr, LS_BAT_X, LS_BAT_Y, LS_BAT_W, LS_BAT_H);
    lv_obj_set_style_radius(bat_shell, 2, 0);
    lv_obj_set_style_border_width(bat_shell, LS_BAT_BORDER, 0);
    lv_obj_set_style_border_color(bat_shell, lv_color_hex(LS_UI_MUTED), 0);
    s_bat_fill = make_box(s_scr, LS_BAT_X + LS_BAT_INSET, LS_BAT_Y + LS_BAT_INSET,
                          LS_BAT_W - 2 * LS_BAT_INSET, LS_BAT_FILL_MAX);
    lv_obj_set_style_radius(s_bat_fill, 1, 0);
    lv_obj_set_style_bg_opa(s_bat_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bat_fill, lv_color_hex(LS_UI_OK), 0);
    // Paint the value saved before the last sleep so the bar is not blank while
    // the gauge finishes its first conversion after the wake.
    ls_status_cache_t cached;
    (void)ls_status_cache_get(&cached);
    update_battery_bar(cached.soc);

    // Link status sits at the right of the same line. Both icons stay hidden
    // until the corresponding online hop has actually succeeded.
    s_wifi_icon = make_label(s_scr, &lv_font_montserrat_14, LS_UI_OK);
    lv_obj_set_style_text_align(s_wifi_icon, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_wifi_icon, 197, 8);
    lv_obj_set_size(s_wifi_icon, 18, 18);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);

    s_deepseek_icon = make_label(s_scr, &lv_font_montserrat_14, LS_UI_OK);
    lv_obj_set_style_text_align(s_deepseek_icon, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_deepseek_icon, 215, 8);
    lv_obj_set_size(s_deepseek_icon, 22, 18);
    lv_label_set_text(s_deepseek_icon, "D");
    update_link_icons();

    lv_obj_t *rule = make_box(s_scr, 12, 30, 216, 2);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(LS_UI_ACCENT), 0);
    lv_obj_t *rule2 = make_box(s_scr, 12, 35, 216, 1);
    lv_obj_set_style_bg_opa(rule2, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rule2, lv_color_hex(LS_UI_RULE), 0);

    s_study = make_box(s_scr, 0, 44, 240, 272);

    s_progress = make_text_label(s_study, 10, 0, 220, 18,
                                 &lv_font_montserrat_14, LS_UI_MUTED, "");
    s_bar = lv_bar_create(s_study);
    lv_obj_set_pos(s_bar, 20, 22);
    lv_obj_set_size(s_bar, 200, 6);
    lv_bar_set_range(s_bar, 0, LS_SESSION_CARDS);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(LS_UI_RULE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(LS_UI_ACCENT),
                              LV_PART_INDICATOR);

    // Dwell countdown for auto-play, in per-mille so the 200 ms tick moves it
    // in visible steps. Hidden until a loop starts.
    s_auto_bar = lv_bar_create(s_study);
    lv_obj_set_pos(s_auto_bar, 20, 226);
    lv_obj_set_size(s_auto_bar, 200, 6);
    lv_bar_set_range(s_auto_bar, 0, 1000);
    lv_bar_set_value(s_auto_bar, 1000, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_auto_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_auto_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_auto_bar, lv_color_hex(LS_UI_RULE),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_auto_bar, lv_color_hex(LS_UI_OK),
                              LV_PART_INDICATOR);
    lv_obj_add_flag(s_auto_bar, LV_OBJ_FLAG_HIDDEN);

    s_word = make_text_label(s_study, 10, 36, 220, 36,
                             &lv_font_montserrat_28, LS_UI_INK, "");
    // Pronunciation sits directly under the word so the card reads
    // spelling -> sound -> meaning. The IPA table is 19 px high; scroll long
    // transcriptions instead of clipping the second line of a wrapped label.
    s_pron = make_text_label(s_study, 12, 74, 216, 20,
                             &langstudy_ipa_16, LS_UI_INK, "");
    lv_label_set_long_mode(s_pron, LV_LABEL_LONG_SCROLL_CIRCULAR);
    // Chinese translation follows the pronunciation. A 16 px CJK line is
    // 20 px high, so 40 px fits two wrapped lines.
    s_translation = make_text_label(s_study, 10, 96, 220, 40,
                                    &langstudy_cjk_16, LS_UI_ACCENT, "");
    s_pos = make_text_label(s_study, 10, 138, 220, 16,
                            &lv_font_montserrat_14, LS_UI_ACCENT, "");
    s_meaning = make_text_label(s_study, 12, 156, 216, 32,
                                &lv_font_montserrat_14, LS_UI_INK, "");
    s_example = make_text_label(s_study, 12, 190, 216, 32,
                                &lv_font_montserrat_14, LS_UI_MUTED, "");
    // The help line is anchored to the bottom of the study area, so the
    // 272 px panel has no unused strip below the last line. It is a member
    // because auto-play swaps the hint for its own stop instruction.
    s_help = make_text_label(s_study, 8, 242, 224, 30, &lv_font_montserrat_14,
                             LS_UI_MUTED,
                             "UP HEAR   OK GOT IT\nDOWN AGAIN   HOLD OK MENU");

    update_study();
    lv_screen_load(s_scr);
    bsp_lvgl_unlock();

    s_last_input_ms = esp_timer_get_time();
    s_battery_ms = s_last_input_ms - LS_BATTERY_PERIOD_MS;
    s_battery_seen = false;
    s_battery_fast_tries = 0;
    s_dimmed = false;
    s_sleep_due = false;
    s_view = LS_VIEW_STUDY;
    return ESP_OK;
}

void ls_ui_on_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    s_last_input_ms = esp_timer_get_time();
    s_sleep_due = false;
    if (s_dimmed) {
        s_dimmed = false;
        bsp_display_backlight(preferred_brightness());
    }

    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    if (!bsp_lvgl_lock(500)) return;

    switch (s_view) {
    case LS_VIEW_STUDY:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN) {
            autoplay_toggle();
        } else if (s_auto.active) {
            // Any real key press hands control back before acting on the key,
            // so auto-play never scores a card behind the learner's back.
            autoplay_stop_and_refresh();
            if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                ls_voice_stop();
                show_menu();
            }
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            ls_voice_stop();
            show_menu();
        } else if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) {
                speak_current();
            } else if (btn == BSP_BTN_OK) {
                answer_card(true);
            } else if (btn == BSP_BTN_DOWN) {
                answer_card(false);
            }
        }
        break;

    case LS_VIEW_SUMMARY:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            show_menu();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            start_new_session();
            show_study();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            show_menu();
        }
        break;

    case LS_VIEW_MENU:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            show_study();
        } else if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) {
                s_menu_index = (s_menu_index + LS_MENU_COUNT - 1) % LS_MENU_COUNT;
                update_menu();
            } else if (btn == BSP_BTN_DOWN) {
                s_menu_index = (s_menu_index + 1) % LS_MENU_COUNT;
                update_menu();
            } else if (btn == BSP_BTN_OK) {
                activate_menu();
            }
        }
        break;

    case LS_VIEW_EXAM:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            show_menu();
        } else if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) {
                s_exam_index = (s_exam_index + LS_EXAM_ROWS - 1) % LS_EXAM_ROWS;
                update_exam();
            } else if (btn == BSP_BTN_DOWN) {
                s_exam_index = (s_exam_index + 1) % LS_EXAM_ROWS;
                update_exam();
            } else if (btn == BSP_BTN_OK) {
                choose_exam();
            }
        }
        break;

    case LS_VIEW_PROGRESS:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            show_menu();
        } else if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) {
                s_progress_index =
                    (s_progress_index + LS_PROGRESS_ROWS - 1) % LS_PROGRESS_ROWS;
                update_progress_view();
            } else if (btn == BSP_BTN_DOWN) {
                s_progress_index = (s_progress_index + 1) % LS_PROGRESS_ROWS;
                update_progress_view();
            } else if (btn == BSP_BTN_OK) {
                activate_progress();
            }
        }
        break;

    case LS_VIEW_BUDDY:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            show_menu();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            feed_buddy();
        }
        break;

    case LS_VIEW_PROFILE:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            show_menu();
        } else if (ev == BSP_BTN_CLICK) {
            if (ls_state()->profile_id[0] != '\0') {
                if (btn == BSP_BTN_OK) show_menu();
            } else if (btn == BSP_BTN_UP) {
                s_profile_name_index =
                    (uint8_t)((s_profile_name_index + LS_BUDDY_NAME_COUNT - 1) %
                              LS_BUDDY_NAME_COUNT);
                update_profile_name();
            } else if (btn == BSP_BTN_DOWN) {
                s_profile_name_index =
                    (uint8_t)((s_profile_name_index + 1) % LS_BUDDY_NAME_COUNT);
                update_profile_name();
            } else if (btn == BSP_BTN_OK) {
                create_profile();
            }
        }
        break;

    case LS_VIEW_DETAIL:
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            show_menu();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            show_menu();
        }
        break;

    case LS_VIEW_SETTINGS:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            if (s_set_adjusting) {
                s_set_adjusting = false;
                update_settings();
            } else {
                show_menu();
            }
        } else if (ev == BSP_BTN_CLICK) {
            if (s_set_adjusting) {
                if (btn == BSP_BTN_UP) {
                    adjust_setting(1);
                } else if (btn == BSP_BTN_DOWN) {
                    adjust_setting(-1);
                } else if (btn == BSP_BTN_OK) {
                    s_set_adjusting = false;
                    update_settings();
                }
            } else if (btn == BSP_BTN_UP) {
                s_set_index =
                    (s_set_index + LS_SETTING_COUNT - 1) % LS_SETTING_COUNT;
                update_settings();
            } else if (btn == BSP_BTN_DOWN) {
                s_set_index = (s_set_index + 1) % LS_SETTING_COUNT;
                update_settings();
            } else if (btn == BSP_BTN_OK) {
                if (s_set_index == LS_SETTING_BACK) {
                    show_menu();
                } else {
                    s_set_adjusting = true;
                    update_settings();
                }
            }
        }
        break;

    case LS_VIEW_STATUS:
        if (!s_status_busy && ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN &&
            s_detail[0]) {
            show_detail_view();
        } else if (!s_status_busy && ev == BSP_BTN_CLICK &&
                   btn == BSP_BTN_OK) {
            if (s_status_next == LS_STATUS_NEXT_STUDY) {
                show_study();
            } else {
                show_menu();
            }
        }
        break;

    case LS_VIEW_PROVISION:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            ls_provision_stop();
            ls_audio_resume();
            // Immediately verify what the phone just configured instead of
            // dropping back to the menu with no feedback.
            if (ls_config_has_wifi(&ls_state()->config)) {
                start_online(LS_ONLINE_CHECK, "WI-FI / DEEPSEEK\nPLEASE WAIT");
            } else {
                show_menu();
            }
        }
        break;

    case LS_VIEW_CONFIRM:
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            do_reset();
        } else if ((ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) ||
                   (ev == BSP_BTN_LONG && btn == BSP_BTN_OK)) {
            show_menu();
        }
        break;
    }

    bsp_lvgl_unlock();
}

void ls_ui_tick(void)
{
    int64_t now = esp_timer_get_time();

    if (now - s_battery_ms >= LS_BATTERY_PERIOD_MS) {
        s_battery_ms = now;
        int soc = bsp_battery_soc();
        if (soc >= 0) {
            s_battery_seen = true;
            ls_status_cache_set_soc(soc);
        } else if (!s_battery_seen && s_battery_fast_tries < LS_BATTERY_FAST_TRIES) {
            // Not ready yet (or still asleep). Try again in 2 s instead of
            // leaving the plain outline up for half a minute.
            s_battery_fast_tries++;
            s_battery_ms = now - LS_BATTERY_PERIOD_MS + LS_BATTERY_FAST_MS;
        }
        // A failed read keeps the last known fill instead of erasing it, which
        // matters right after a wake while the chip is still computing.
        if (soc >= 0 && bsp_lvgl_lock(200)) {
            update_battery_bar(soc);
            bsp_lvgl_unlock();
        }
    }

    ls_online_msg_t msg;
    if (ls_online_poll(&msg)) {
        if (bsp_lvgl_lock(500)) {
            handle_online(&msg);
            bsp_lvgl_unlock();
        } else {
            free(msg.pack);
            free(msg.progress);
        }
    }

    // Auto-play advances the study card on this same 200 ms tick. Treat the
    // loop as activity so the panel never dims or deep-sleeps mid-lesson.
    if (s_auto.active) {
        if (s_view != LS_VIEW_STUDY) {
            ls_autoplay_stop(&s_auto);
        } else {
            s_last_input_ms = now;
            uint32_t dwell = auto_dwell_ms();
            if (ls_autoplay_poll(&s_auto, now, dwell)) s_auto_redraw = true;
            if (bsp_lvgl_lock(200)) {
                if (s_auto_redraw) {
                    s_auto_redraw = false;
                    update_study();
                    const ls_word_t *word = display_word();
                    if (word) (void)ls_voice_speak(word->text, word->pron);
                } else if (s_auto_bar) {
                    lv_bar_set_value(
                        s_auto_bar,
                        (int32_t)ls_autoplay_remaining_permille(&s_auto, now,
                                                                dwell),
                        LV_ANIM_OFF);
                }
                bsp_lvgl_unlock();
            }
        }
    }

    bool sleepable_view = (s_view == LS_VIEW_STUDY ||
                           s_view == LS_VIEW_SUMMARY ||
                           s_view == LS_VIEW_MENU ||
                           s_view == LS_VIEW_BUDDY ||
                           s_view == LS_VIEW_PROFILE);
    if (sleepable_view && !ls_online_busy() && !ls_provision_active()) {
        int64_t idle = now - s_last_input_ms;
        if (!s_dimmed && idle >= LS_IDLE_DIM_MS) {
            s_dimmed = true;
            bsp_display_backlight(20);
        }
        if (idle >= LS_IDLE_SLEEP_MS) {
            s_sleep_due = true;
        }
    }
}

bool ls_ui_idle_deep_sleep_due(void)
{
    return s_sleep_due;
}

void ls_ui_shutdown(void)
{
    ls_autoplay_stop(&s_auto);
    if (!bsp_lvgl_lock(1000)) return;
    if (s_modal) {
        lv_obj_delete(s_modal);
        s_modal = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_study = NULL;
    s_auto_bar = NULL;
    s_help = NULL;
    s_translation = NULL;
    s_bat_fill = NULL;
    s_wifi_icon = NULL;
    s_deepseek_icon = NULL;
    s_set_hint = NULL;
    for (int i = 0; i < LS_MENU_COUNT; i++) {
        s_menu_rows[i] = NULL;
        s_menu_labels[i] = NULL;
    }
    for (int i = 0; i < LS_EXAM_ROWS; i++) {
        s_exam_rows[i] = NULL;
        s_exam_labels[i] = NULL;
    }
    for (int i = 0; i < LS_SETTING_COUNT; i++) {
        s_set_rows[i] = NULL;
        s_set_labels[i] = NULL;
    }
    bsp_lvgl_unlock();
}
