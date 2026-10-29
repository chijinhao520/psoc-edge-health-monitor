/**
 * @file  ui_tab_monitor.c
 * @brief Tab 1 — real-time health dashboard.
 *
 * Layout (top → bottom, vertical flex):
 *   状态栏 → 引导体检 → 实时指标卡片 → 环境。内容可纵向滚动。
 *
 * A 1-second lv_timer reads ui_data and refreshes all labels / arcs.
 */
#include "ui_tab_monitor.h"
#include "ui_common.h"
#include "ui_data.h"
#include "checkup_widgets.h"
static checkup_widgets_t s_guide;

/* ====== widget handles for periodic refresh ====== */
static struct {
    /* glucose（已从 hero 降级为普通卡片，与其余指标平级） */
    lv_obj_t *glu_val;
    /* heart rate */
    lv_obj_t *hr_arc;
    lv_obj_t *hr_val;
    /* SpO2 */
    lv_obj_t *spo2_arc;
    lv_obj_t *spo2_val;
    /* body temperature (MLX90614) */
    lv_obj_t *temp_val;
    /* BMR */
    lv_obj_t *bmr_val;
    /* blood flow */
    lv_obj_t *gsr_val;
    /* environment */
    lv_obj_t *env_temp;
    lv_obj_t *env_humi;
    /* status bar */
    lv_obj_t *status_dot;
    lv_obj_t *status_label;
} w;

/* ====== glucose status helpers ======
 * 血糖降级为普通卡片后，"偏低/正常/偏高/危险"文字随 hero 一起去掉 ——
 * 血糖是标了实验性的参考值，给它下"正常"这种结论性判词本就过重。
 * 保留配色：数值本身用颜色暗示区间即可。分级文字如需回归，见体检报告页。 */

static lv_color_t glucose_status_color(float v)
{
    if (v < GLUCOSE_LOW)     return UI_COLOR_WARN;
    if (v <= GLUCOSE_HIGH)   return UI_COLOR_OK;
    if (v <= GLUCOSE_DANGER) return UI_COLOR_WARN;
    return UI_COLOR_DANGER;
}

/* ====== builder helpers ====== */

/* ---------- status bar (32 px) ---------- */
static void build_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_CARD_FULL_W, 32);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* title */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title,
        "\xe5\x81\xa5\xe5\xba\xb7\xe7\x9b\x91\xe6\xb5\x8b"   /* 健康监测 */
        "\xe7\xbb\x88\xe7\xab\xaf");                           /* 终端 */
    lv_obj_set_style_text_font(title, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    w.status_label=lv_label_create(bar);
    lv_label_set_text(w.status_label,"采集端未连接");
    lv_obj_set_style_text_font(w.status_label,&ui_font_Subtitle,LV_PART_MAIN);
    lv_obj_set_style_text_color(w.status_label,UI_COLOR_TEXT_SEC,LV_PART_MAIN);
    /* 只有实际收到持续更新的采集核心跳才亮绿。 */
    lv_obj_t *dot = lv_obj_create(bar);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_bg_color(dot, UI_COLOR_WARN, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    w.status_dot = dot;
}

/* ---------- a single vital-sign card with arc ---------- */
static lv_obj_t *build_arc_card(lv_obj_t *parent, const char *title,
                                 const char *unit_str, lv_color_t color,
                                 int amin, int amax, int aval,
                                 lv_obj_t **out_arc, lv_obj_t **out_val)
{
    lv_obj_t *card = ui_card_create(parent, UI_CARD_HALF_W, 140);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);

    /* colored left-border accent */
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, color, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);

    /* title */
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    /* arc + value overlay container */
    lv_obj_t *wrap = lv_obj_create(card);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, 80, 80);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *arc = ui_arc_create(wrap, 80, color, amin, amax, aval);
    lv_obj_center(arc);
    *out_arc = arc;

    /* value label centered inside arc */
    lv_obj_t *v = lv_label_create(wrap);
    lv_label_set_text_fmt(v, "%d", aval);
    lv_obj_set_style_text_font(v, &ui_font_Number_big, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, color, LV_PART_MAIN);
    lv_obj_center(v);
    *out_val = v;

    /* unit */
    lv_obj_t *u = lv_label_create(card);
    lv_label_set_text(u, unit_str);
    lv_obj_set_style_text_font(u, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(u, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    return card;
}

/* ---------- a value-only card (no arc) ---------- */
static lv_obj_t *build_value_card(lv_obj_t *parent, const char *title,
                                   const char *init_val, const char *unit_str,
                                   lv_color_t color, lv_obj_t **out_val)
{
    lv_obj_t *card = ui_card_create(parent, UI_CARD_HALF_W, 140);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);

    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, color, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, init_val);
    lv_obj_set_style_text_font(v, &ui_font_Number_big, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, color, LV_PART_MAIN);
    *out_val = v;

    lv_obj_t *u = lv_label_create(card);
    lv_label_set_text(u, unit_str);
    lv_obj_set_style_text_font(u, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(u, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    return card;
}

/* ---------- two-card row container ---------- */
static lv_obj_t *build_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, UI_PAD_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* ---------- environment bar ---------- */
static void build_env_bar(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, 72);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, UI_COLOR_ENV, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);

    /* env title */
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t,
        "\xe7\x8e\xaf\xe5\xa2\x83");  /* 环境 */
    lv_obj_set_style_text_font(t, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_ENV, LV_PART_MAIN);

    /* temp —— 初始 "--"：IPC 未就绪时不能挂着编造的读数 */
    w.env_temp = lv_label_create(card);
    lv_label_set_text(w.env_temp, "--");
    lv_obj_set_style_text_font(w.env_temp, &ui_font_H1, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.env_temp, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    /* humidity */
    w.env_humi = lv_label_create(card);
    lv_label_set_text(w.env_humi, "--");
    lv_obj_set_style_text_font(w.env_humi, &ui_font_H1, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.env_humi, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
}

/* ====== periodic refresh ====== */

static void refresh_cb(lv_timer_t *t)
{
    checkup_widgets_refresh(&s_guide);
    (void)t;
    const struct ui_health_data *d = ui_data_get();
    char buf[32];
    lv_obj_set_style_bg_color(w.status_dot,d->sensor_link_valid ? UI_COLOR_OK : UI_COLOR_WARN,LV_PART_MAIN);
    lv_label_set_text(w.status_label,d->sensor_link_valid ? "采集端已连接" : "采集端未连接");

    /*
     * 测量值一律先看 valid：没测过就显示 "--"，绝不拿初值/残值充数。
     *
     * 这里曾经是无条件 lv_label_set_text(d->glucose)，配合 ui_data.c 里
     * glucose=5.2/heart_rate=75 的假初值，导致一次都没测过、屏幕上照样
     * 有"血糖 5.2 正常""心率 75"。体检设备编数是硬伤，宁可显示 "--"。
     */

    /* heart rate */
    if (d->hr_valid) {
        lv_arc_set_value(w.hr_arc, d->heart_rate);
        lv_label_set_text_fmt(w.hr_val, "%d", d->heart_rate);
    } else {
        lv_arc_set_value(w.hr_arc, 0);
        lv_label_set_text(w.hr_val, "--");
    }

    /* SpO2 */
    if (d->spo2_valid) {
        lv_arc_set_value(w.spo2_arc, (int)(d->spo2));
        ui_fmt_f1(buf, sizeof(buf), d->spo2);
        lv_label_set_text(w.spo2_val, buf);
    } else {
        lv_arc_set_value(w.spo2_arc, 0);
        lv_label_set_text(w.spo2_val, "--");
    }

    /* 体温（MLX90614，1Hz 实时） */
    if (d->body_temp_valid) {
        ui_fmt_f1(buf, sizeof(buf), d->body_temp);
        lv_label_set_text(w.temp_val, buf);
    } else {
        lv_label_set_text(w.temp_val, "--");
    }

    /* BMR：只在一次测量完成后更新，未测显示 "--" */
    if (d->bmr_valid) {
        ui_fmt_f1(buf, sizeof(buf), d->bmr);
        lv_label_set_text(w.bmr_val, buf);
    } else {
        lv_label_set_text(w.bmr_val, "--");
    }

    /* 血糖（辅 AI，参考值） */
    if (d->glucose_valid) {
        ui_fmt_f1(buf, sizeof(buf), d->glucose);
        lv_label_set_text(w.glu_val, buf);
        lv_obj_set_style_text_color(w.glu_val,
            glucose_status_color(d->glucose), LV_PART_MAIN);
    } else {
        lv_label_set_text(w.glu_val, "--");
        lv_obj_set_style_text_color(w.glu_val, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
    }

    /* GSR是相对个人基线的工程指数，不是绝对应激诊断。 */
    if (d->gsr_valid) {
        ui_fmt_f1(buf, sizeof(buf), d->gsr_stress_index);
        lv_label_set_text(w.gsr_val, buf);
    } else lv_label_set_text(w.gsr_val, "--");

    /* environment（仅外接 SHT30，有效性由采集核发布） */
    if (d->env_valid) {
        char tmp[16];
        ui_fmt_f1(tmp, sizeof(tmp), d->env_temp);
        lv_snprintf(buf, sizeof(buf), "%s\xc2\xb0""C", tmp);
        lv_label_set_text(w.env_temp, buf);
        ui_fmt_f1(tmp, sizeof(tmp), d->env_humidity);
        lv_snprintf(buf, sizeof(buf), "%s%%RH", tmp);
        lv_label_set_text(w.env_humi, buf);
    } else {
        lv_label_set_text(w.env_temp, "--");
        lv_label_set_text(w.env_humi, "--");
    }
}

/* ====== public entry ====== */

void ui_tab_monitor_create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(parent, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(parent, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, UI_PAD_GAP, LV_PART_MAIN);

    /* 1) status bar */
    build_status_bar(parent);
    checkup_widgets_create(&s_guide,parent,1);
    lv_obj_set_scroll_dir(parent,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent,LV_SCROLLBAR_MODE_AUTO);

    /*
     * 3~4) 统一数据网格。
     *
     * 血糖原先独占一个 175px 的 hero 卡（那是"PPG 无创血糖终端"旧定位的
     * 遗留）。5/28 战略转向后血糖已降级为"辅 AI / 参考值"，主打的是
     * 一次测量出一组体检数据 —— 让它跟其余指标平级更诚实，也免得评委
     * 把注意力全放在最弱的那一项上。
     *
     * 一屏放下全部测量项：心率 / 血氧 / 体温 / 代谢 / 血糖。
     */
    lv_obj_t *row1 = build_row(parent);
    build_arc_card(row1,
        "\xe5\xbf\x83\xe7\x8e\x87",   /* 心率 */
        "BPM", UI_COLOR_HEART_RATE,
        40, 180, 0,
        &w.hr_arc, &w.hr_val);
    build_arc_card(row1,
        "\xe8\xa1\x80\xe6\xb0\xa7",   /* 血氧 */
        "%", UI_COLOR_SPO2,
        85, 100, 0,
        &w.spo2_arc, &w.spo2_val);

    /* 体温：MLX90614 已迁回 mux CH2 并恢复采集，重新上屏 */
    lv_obj_t *row2 = build_row(parent);
    build_value_card(row2,
        "红外温度",
        "--", "\xc2\xb0""C",          /* °C */
        UI_COLOR_TEMP, &w.temp_val);
    build_value_card(row2,
        "\xe4\xbb\xa3\xe8\xb0\xa2",   /* 代谢 */
        "--", "kcal/h",
        UI_COLOR_BMR, &w.bmr_val);

    /* 血糖：与其余指标平级的一张卡，不再是 hero */
    lv_obj_t *row3 = build_row(parent);
    build_value_card(row3,
        "\xe8\xa1\x80\xe7\xb3\x96",   /* 血糖 */
        "--", "mmol/L",
        UI_COLOR_GLUCOSE, &w.glu_val);
    build_value_card(row3, "应激", "--", "GSR 0-100",
                     UI_COLOR_SPO2, &w.gsr_val);

    /* 5) environment */
    build_env_bar(parent);

    /* 6) start 1-second refresh */
    lv_timer_create(refresh_cb, 1000, NULL);
}
