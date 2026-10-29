/**
 * @file  ui_main.c
 * @brief Override lv_user_gui_init() — create bottom TabView with 3 tabs.
 *
 * 导航栏为什么不用 TabView 自带的 tab bar（2026-07-17）：
 *   LVGL 9.2 的 tabview 里 bar 是 child 0、content 是 child 1，绘制顺序
 *   固定为 bar 先画、content 后画。报告页是唯一内容超一屏要滚动裁剪的页，
 *   实测初次切入时页面内容会画到 bar 上把切换键"挡住"（点击仍有效——
 *   hit-test 按对象坐标，说明只是绘制越界，疑似 VG-Lite 路径裁剪不生效）。
 *   把 bar 挪出 tabview、挂在 screen 顶层最后创建，无论内容怎么越界绘制,
 *   导航栏永远最后画、永远可见 —— 对根因鲁棒。
 *   注意不能用 lv_obj_move_foreground(bar)：lv_tabview_get_tab_bar/content
 *   靠 child 索引取对象，动了顺序 tab 切换就坏。
 */
#include "lvgl.h"
#include "ui_common.h"
#include "ui_data.h"
#include "ui_tab_monitor.h"
#include "ui_tab_settings.h"
#include "ui_tab_report.h"

#define NAV_TAB_CNT 3

static lv_obj_t *s_tv;
static lv_obj_t *s_nav_lbl[NAV_TAB_CNT];

/* 高亮当前页：选中 = 蓝，未选 = 次级灰（对应原 bar 的 checked 视觉） */
static void nav_highlight(uint32_t act)
{
    for (uint32_t i = 0; i < NAV_TAB_CNT; i++) {
        lv_obj_set_style_text_color(s_nav_lbl[i],
            (i == act) ? UI_COLOR_SPO2 : UI_COLOR_TEXT_SEC, LV_PART_MAIN);
    }
}

static void nav_btn_cb(lv_event_t *e)
{
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_tabview_set_active(s_tv, idx, LV_ANIM_OFF);
    nav_highlight(idx);
}

void ui_show_report(void)
{
    if (!s_tv) return;
    lv_tabview_set_active(s_tv,1,LV_ANIM_OFF);
    nav_highlight(1);
}

/* 手指横向滑动切页走 tabview 内部 snap 逻辑，不经过按钮回调 ——
 * 靠 VALUE_CHANGED 把高亮同步回来 */
static void tv_value_changed_cb(lv_event_t *e)
{
    (void)e;
    nav_highlight(lv_tabview_get_tab_active(s_tv));
}

static void build_nav_bar(lv_obj_t *scr)
{
    static const char *names[NAV_TAB_CNT] = {
        "\xe7\x9b\x91\xe6\xb5\x8b",   /* 监测 */
        "\xe6\x8a\xa5\xe5\x91\x8a",   /* 报告 */
        "\xe8\xae\xbe\xe7\xbd\xae",   /* 设置 */
    };

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_SCREEN_W, UI_TAB_BAR_H);
    lv_obj_set_pos(bar, 0, UI_SCREEN_H - UI_TAB_BAR_H);
    lv_obj_set_style_bg_color(bar, UI_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, UI_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar, UI_VRAM_LEFT_MARGIN, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (uint32_t i = 0; i < NAV_TAB_CNT; i++) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, nav_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_font(lbl, &ui_font_Subtitle, LV_PART_MAIN);
        lv_obj_center(lbl);
        s_nav_lbl[i] = lbl;
    }

    nav_highlight(0);
}

/* Override the weak lv_user_gui_init() in lv_rt_thread_port.c */
void lv_user_gui_init(void)
{
    /* ---- page background = 黑色 ----
     * DC 图形层 pos_x=35，MIPI 输出前 35 列 = DC 黑底。
     * 面板 PLL 环绕时，环绕像素 = 黑色 → 融入边框不可见。 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ---- health-data provider（IPC 500ms 轮询，见 ui_data.c） ---- */
    ui_data_init();

    /* ---- TabView（仅当滑动切页容器用，自带 bar 隐藏，见文件头） ---- */
    lv_obj_t *tv = lv_tabview_create(scr);
    s_tv = tv;
    lv_tabview_set_tab_bar_position(tv, LV_DIR_BOTTOM);
    lv_obj_set_size(tv, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(tv, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(lv_tabview_get_tab_bar(tv), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(tv, tv_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- add 3 tabs（血糖页已并入：监测页有实时卡、报告页有评估行。
     *      5/28 战略转向后血糖 = 辅助参考值，独占一页是旧定位遗留） ---- */
    lv_obj_t *tab1 = lv_tabview_add_tab(tv, "\xe7\x9b\x91\xe6\xb5\x8b");   /* 监测 */
    lv_obj_t *tab2 = lv_tabview_add_tab(tv, "\xe6\x8a\xa5\xe5\x91\x8a");   /* 报告 */
    lv_obj_t *tab3 = lv_tabview_add_tab(tv, "\xe8\xae\xbe\xe7\xbd\xae");   /* 设置 */

    /* bar 隐藏后 content 占满整个 800 高。给 content 留出 pad_bottom =
     * 导航栏高度，tab page(高 = content 内容区 100%)恢复成 740，与原布局
     * 完全一致 —— 三个页面文件不需要感知这次导航栏重构。 */
    lv_obj_t *tv_content = lv_tabview_get_content(tv);
    lv_obj_set_style_pad_all(tv_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(tv_content, UI_TAB_BAR_H, LV_PART_MAIN);

    /* tab content: 左侧额外补偿 DC 层偏移 */
    lv_obj_t *tabs[] = {tab1, tab2, tab3};
    for (int i = 0; i < NAV_TAB_CNT; i++) {
        lv_obj_set_style_bg_color(tabs[i], UI_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tabs[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tabs[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(tabs[i], UI_PAD_SIDE + UI_VRAM_LEFT_MARGIN, LV_PART_MAIN);
        lv_obj_set_style_pad_right(tabs[i], UI_PAD_SIDE, LV_PART_MAIN);
    }

    ui_tab_monitor_create(tab1);
    ui_tab_report_create(tab2);
    ui_tab_settings_create(tab3);

    /* ---- 自绘底部导航栏：必须在 tv 之后创建（z-order = 绘制顺序） ---- */
    build_nav_bar(scr);

    /* VRAM 环绕防护：左侧 35px 涂黑色
     * 面板 PLL 偏移 ~35px → VRAM[0..N] 环绕到屏幕右边缘
     * 黑色融入物理边框，不可见
     * 必须最后 move_foreground —— 要盖住导航栏左端的白底 */
    lv_obj_t *guard = lv_obj_create(scr);
    lv_obj_remove_style_all(guard);
    lv_obj_set_size(guard, UI_VRAM_LEFT_MARGIN, UI_SCREEN_H);
    lv_obj_set_pos(guard, 0, 0);
    lv_obj_set_style_bg_color(guard, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(guard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_move_foreground(guard);
}
