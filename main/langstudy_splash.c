// main/langstudy_splash.c
// Cyber-pixel launch screen: the dedication plus the family artwork generated
// by tools/gen_splash.py, a five-second loading bar and a caption.
#include "langstudy_splash.h"

#include "langstudy_splash_img.h"

#include "bsp_display.h"
#include "lvgl.h"

#define SPLASH_BG 0x050614
#define SPLASH_TEXT 0xF7F1E3
#define SPLASH_MUTED 0x8F86B8
#define SPLASH_ACCENT 0xFF48BE

static lv_obj_t *s_scr;
static lv_obj_t *s_bar;
static lv_obj_t *s_caption;

static lv_obj_t *splash_label(lv_obj_t *parent, const lv_font_t *font,
                              uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

esp_err_t ls_splash_show(void)
{
    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;

    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) {
        bsp_lvgl_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(SPLASH_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *img = lv_image_create(scr);
    lv_image_set_src(img, &langstudy_splash_img);
    lv_obj_set_pos(img, 0, 52);

    lv_obj_t *dedication =
        splash_label(scr, &lv_font_montserrat_14, SPLASH_TEXT, 12, 8, 216, 40);
    lv_label_set_text(dedication,
                      "Gift for my beloved daughters, April and Abby.");

    s_bar = lv_bar_create(scr);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_bar, 20, 252);
    lv_obj_set_size(s_bar, 200, 6);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x1B1230), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(SPLASH_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);

    s_caption =
        splash_label(scr, &lv_font_montserrat_14, SPLASH_MUTED, 0, 266, 240, 20);
    lv_label_set_text(s_caption, "LOADING...");

    s_scr = scr;
    lv_screen_load(s_scr);
    bsp_lvgl_unlock();
    return ESP_OK;
}

void ls_splash_set_progress(uint32_t permille)
{
    if (permille > 1000) permille = 1000;
    if (!s_bar) return;
    if (!bsp_lvgl_lock(1000)) return;
    lv_bar_set_value(s_bar, (int32_t)permille, LV_ANIM_OFF);
    bsp_lvgl_unlock();
}

void ls_splash_set_caption(const char *text)
{
    if (!text || !s_caption) return;
    if (!bsp_lvgl_lock(1000)) return;
    lv_label_set_text(s_caption, text);
    bsp_lvgl_unlock();
}

void ls_splash_hide(void)
{
    if (!s_scr) return;
    if (!bsp_lvgl_lock(1000)) return;
    // The caller has already loaded the main screen, so this one is inactive.
    lv_obj_delete(s_scr);
    s_scr = NULL;
    s_bar = NULL;
    s_caption = NULL;
    bsp_lvgl_unlock();
}
