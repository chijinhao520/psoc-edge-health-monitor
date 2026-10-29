#ifndef __UI_COMMON_H__
#define __UI_COMMON_H__

#include "lvgl.h"

/* ====== Color Palette ====== */
#define UI_COLOR_BG             lv_color_hex(0xF0F4F8)
#define UI_COLOR_CARD           lv_color_hex(0xFFFFFF)
#define UI_COLOR_TEXT_PRI       lv_color_hex(0x1A202C)
#define UI_COLOR_TEXT_SEC       lv_color_hex(0x718096)
#define UI_COLOR_DIVIDER        lv_color_hex(0xE2E8F0)

#define UI_COLOR_GLUCOSE        lv_color_hex(0x00897B)
#define UI_COLOR_HEART_RATE     lv_color_hex(0xE53935)
#define UI_COLOR_SPO2           lv_color_hex(0x1E88E5)
#define UI_COLOR_TEMP           lv_color_hex(0xFB8C00)
#define UI_COLOR_BMR            lv_color_hex(0x7B1FA2)
#define UI_COLOR_ENV            lv_color_hex(0x43A047)

#define UI_COLOR_OK             lv_color_hex(0x4CAF50)
#define UI_COLOR_WARN           lv_color_hex(0xFF9800)
#define UI_COLOR_DANGER         lv_color_hex(0xE53935)

/* ====== Layout Constants ====== */
#define UI_SCREEN_W             512
#define UI_SCREEN_H             800
#define UI_TAB_BAR_H            60
#define UI_PAD_SIDE             16
#define UI_VRAM_LEFT_MARGIN     35   /* DC 图形层 pos_x 偏移（吸收面板 PLL 环绕像素） */
#define UI_PAD_GAP              12
#define UI_CARD_RADIUS          16
#define UI_CARD_FULL_W          (480 - UI_VRAM_LEFT_MARGIN - 2 * UI_PAD_SIDE)
#define UI_CARD_HALF_W          ((UI_CARD_FULL_W - UI_PAD_GAP) / 2)

/* ====== API ====== */

/**
 * Create a white rounded card with subtle shadow.
 * @param parent  Parent container
 * @param w       Width (use UI_CARD_FULL_W or UI_CARD_HALF_W)
 * @param h       Height in pixels
 * @return        The card object
 */
lv_obj_t *ui_card_create(lv_obj_t *parent, int32_t w, int32_t h);

/**
 * Create a 270-degree arc gauge (non-clickable).
 * @param parent   Parent container
 * @param size     Diameter in pixels
 * @param color    Indicator color
 * @param min_val  Range minimum
 * @param max_val  Range maximum
 * @param cur_val  Initial value
 * @return         The arc object
 */
lv_obj_t *ui_arc_create(lv_obj_t *parent, int size, lv_color_t color,
                         int min_val, int max_val, int cur_val);

/**
 * Format float with 1 decimal place using integer math only.
 * Avoids %f which requires rt_vsnprintf_full (not enabled on M55).
 */
static inline void ui_fmt_f1(char *buf, int sz, float val)
{
    int neg = (val < 0.0f);
    if (neg) val = -val;
    int v = (int)(val * 10.0f + 0.5f);
    lv_snprintf(buf, sz, "%s%d.%d", neg ? "-" : "", v / 10, v % 10);
}

#endif /* __UI_COMMON_H__ */
