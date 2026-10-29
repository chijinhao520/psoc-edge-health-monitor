#include "ui_common.h"

lv_obj_t *ui_card_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, UI_CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, UI_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x90A4AE), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(card, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *ui_arc_create(lv_obj_t *parent, int size, lv_color_t color,
                         int min_val, int max_val, int cur_val)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, min_val, max_val);
    lv_arc_set_value(arc, cur_val);

    /* Indicator (filled portion) */
    lv_obj_set_style_arc_width(arc, size / 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    /* Background track */
    lv_obj_set_style_arc_width(arc, size / 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xE8EDF2), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);

    /* Hide knob */
    lv_obj_set_style_pad_all(arc, -2, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}
