/**
 * @file  ui_tab_report.c
 * @brief Tab 3 — 体检报告(把各项指标解读成 评级 / 状态 / 建议 的一页式报告)
 *
 * 这一页是"数字仪表盘 → 体检报告"的产品层：每项指标带 数值 + 参考范围 +
 * 状态徽标，顶部综合评估给分级 + 一句话汇总，底部健康建议按临床严重度列出
 * 复测/就医建议 + 免责声明。所有"正不正常 / 给什么建议 / 评几档"的判断都在
 * health_report.c(纯 C，可测)，本文件只负责渲染。
 *
 * 诚实口径：
 *   - 呼吸/血糖/PAT 标"实验性"，异常也只给复测/参考级建议，绝不出就医结论；
 *   - 心律不译中文病名，只显示 N/S/V/F + 早搏负荷，诊断留给医生；
 *   - 数据不足(valid==0)一律显示 "--" / "未测"，不编造数值。
 */
#include "ui_tab_report.h"
#include "ui_common.h"
#include "ui_data.h"
#include "ui_cn_font.h"
#include "checkup_widgets.h"
static checkup_widgets_t s_guide_report;
static lv_obj_t *s_ai_header, *s_ai_card;
#include "../health_report.h"
#include "../advice_local.h"
#include "../mqtt_upload.h"   /* mqtt_advice_ready/request：按键联网走云端 */
#include <rtthread.h>
#include <string.h>

/* ---------- 本文件用到的中文(UTF-8 hex 转义，项目约定) ---------- */
#define TXT_TITLE   "\xe4\xbd\x93\xe6\xa3\x80\xe6\x8a\xa5\xe5\x91\x8a"   /* 体检报告 */
#define TXT_ASSESS  "\xe7\xbb\xbc\xe5\x90\x88\xe8\xaf\x84\xe4\xbc\xb0"   /* 综合评估 */
#define TXT_VITALS  "\xe7\x94\x9f\xe5\x91\xbd\xe4\xbd\x93\xe5\xbe\x81"   /* 生命体征 */
#define TXT_ADVICE  "\xe5\x81\xa5\xe5\xba\xb7\xe5\xbb\xba\xe8\xae\xae"   /* 健康建议 */
#define TXT_RHYTHM  "\xe5\xbf\x83\xe5\xbe\x8b"                           /* 心律 */
#define TXT_HR      "\xe5\xbf\x83\xe7\x8e\x87"                           /* 心率 */
#define TXT_SPO2    "\xe8\xa1\x80\xe6\xb0\xa7"                           /* 血氧 */
#define TXT_RESP    "\xe5\x91\xbc\xe5\x90\xb8"                           /* 呼吸 */
#define TXT_STRESS  "\xe5\xba\x94\xe6\xbf\x80"                           /* 应激 */
#define TXT_GLU     "\xe8\xa1\x80\xe7\xb3\x96"                           /* 血糖 */
#define TXT_REF     "\xe5\x8f\x82\xe8\x80\x83"                           /* 参考 */
#define TXT_EXP     "\xe5\xae\x9e\xe9\xaa\x8c\xe6\x80\xa7"               /* 实验性 */
#define TXT_BPM_CN  "\xe6\xac\xa1/\xe5\x88\x86"                          /* 次/分 */

/* AI 健康建议正文初始占位: "正在等待云端健康分析" */
#define TXT_AI_WAIT "\xe6\xad\xa3\xe5\x9c\xa8\xe7\xad\x89\xe5\xbe\x85\xe4\xba\x91\xe7\xab\xaf\xe5\x81\xa5\xe5\xba\xb7\xe5\x88\x86\xe6\x9e\x90"

/* 列表行(HR/SpO2/呼吸/HRV/应激/血糖/PAT)对应的 health_report 指标下标，
 * PAT 无对应解读项(无血压模型) → -1，只按 valid 显示、不判状态。 */
#define REPORT_ROWS  7
#define ROW_PAT      6
static const int ROW_METRIC[REPORT_ROWS] = {
    HRPT_M_HR, HRPT_M_SPO2, HRPT_M_RESP, HRPT_M_HRV, HRPT_M_STRESS, HRPT_M_GLU, -1
};

typedef struct { lv_obj_t *val; lv_obj_t *badge; } rrow_t;

static struct {
    lv_obj_t *status_dot;
    /* 综合评估 hero */
    lv_obj_t *score_arc;
    lv_obj_t *score_num;
    lv_obj_t *grade;
    lv_obj_t *summary;
    /* 心律旗舰卡 */
    lv_obj_t *rhy_letter;
    lv_obj_t *rhy_badge;
    lv_obj_t *rhy_pvc;
    /* 指标列表 */
    rrow_t    row[REPORT_ROWS];
    /* 健康建议 */
    lv_obj_t *adv[HRPT_MAX_ADVICE];
    /* AI 健康建议(云端 LLM, Tiny TTF 显示任意中文) */
    lv_obj_t *ai_advice;
} w;

/* ---- AI 建议跨线程传递: MQTT 线程写 buffer+dirty, UI 定时器读并刷 ---- */
/*
 * AI 建议正文缓冲。
 *
 * ★ 512 → 1536（2026-07-17，对抗审 memory lens 实测抓出）：
 * 原 512 是给 MQTT 云端文本定的。设备端生成(advice_local.c)接进来后复用了它，
 * 而实测**最平常的场景**（心率+血氧+体温全正常 + 1 条建议）正文就有约 900 字节，
 * 数据齐全时更多 —— 于是：
 *     pos 421→511  "...呼吸率与血糖为实验性指标，仅供参考。"   ← 句中截断
 *     pos 511→511  "本报告...不作为医疗诊断依据。"              ← 整句 0 字节，消失
 * **医疗免责声明在常规使用中会从屏幕上消失**，这是体检类作品最不该丢的一句。
 * 且截断按字节、不认 UTF-8 边界，换组数值就可能切碎汉字渲染出乱码。
 *
 * 为什么之前没发现：advice_local.h 写明"建议 >= 1024"，但它的硬门槛
 * ADVICE_LOCAL_MIN_BUF 也是 512 —— `cap < 512` 判不出 `cap == 512`，
 * 文档说 1024、检查放行 512、调用点给 512，三处自相矛盾且编译期无征兆。
 * （中文 UTF-8 一字 3 字节，1536 ≈ 500 汉字，留足余量。）
 *
 * MQTT 路径(mqtt_upload.c:128)共用 s_ai_buf，加大对它只有好处。
 */
#define AI_ADVICE_BUF_LEN 1536
static char         s_ai_buf[AI_ADVICE_BUF_LEN];
static volatile int s_ai_dirty = 0;
static rt_mutex_t   s_ai_lock = RT_NULL;

/* ---------- 通用小工具 ---------- */
static lv_color_t level_color(int level)
{
    switch (level) {
        case HRPT_LV_OK:     return UI_COLOR_OK;
        case HRPT_LV_WARN:   return UI_COLOR_WARN;
        case HRPT_LV_DANGER: return UI_COLOR_DANGER;
        default:             return UI_COLOR_TEXT_SEC;
    }
}

static const char *arr_class_letter(int cls)
{
    static const char *letters[4] = { "N", "S", "V", "F" };
    if (cls < 0 || cls > 3) return "-";
    return letters[cls];
}

/* 徽标 chip：小圆角 + 淡底 + 彩字 */
static lv_obj_t *make_badge(lv_obj_t *parent)
{
    lv_obj_t *b = lv_label_create(parent);
    lv_label_set_text(b, "--");
    lv_obj_set_style_text_font(b, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(b, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(b, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(b, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(b, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(b, 3, LV_PART_MAIN);
    return b;
}

static void set_badge(lv_obj_t *b, const char *text, lv_color_t c)
{
    lv_label_set_text(b, text);
    lv_obj_set_style_text_color(b, c, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, c, LV_PART_MAIN);
}

/* ---------- status bar ---------- */
static void build_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_CARD_FULL_W, 32);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, TXT_TITLE);
    lv_obj_set_style_text_font(title, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    lv_obj_t *dot = lv_obj_create(bar);
    w.status_dot=dot;
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_bg_color(dot, UI_COLOR_OK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
}

/* ---------- 综合评估 hero ---------- */
static void build_assess_hero(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, 168);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 20, LV_PART_MAIN);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, UI_COLOR_BMR, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);

    /* 左：得分环，中心叠数字 */
    lv_obj_t *wrap = lv_obj_create(card);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, 128, 128);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    w.score_arc = ui_arc_create(wrap, 128, UI_COLOR_BMR, 0, 100, 0);
    lv_obj_center(w.score_arc);

    w.score_num = lv_label_create(wrap);
    lv_label_set_text(w.score_num, "--");
    lv_obj_set_style_text_font(w.score_num, &ui_font_Number_big, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.score_num, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    lv_obj_center(w.score_num);

    /* 右：标题 + 评级词 + 汇总 */
    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, TXT_ASSESS);
    lv_obj_set_style_text_font(t, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    w.grade = lv_label_create(col);
    lv_label_set_text(w.grade, "--");
    lv_obj_set_style_text_font(w.grade, &ui_font_H1, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.grade, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    w.summary = lv_label_create(col);
    lv_label_set_text(w.summary, "");
    lv_obj_set_style_text_font(w.summary, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.summary, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
}

/* ---------- 心律旗舰卡(主 AI ①) ---------- */
static void build_rhythm_card(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, 96);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, UI_COLOR_HEART_RATE, LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);

    /* 左：名称 + PVC 副行 */
    lv_obj_t *lc = lv_obj_create(card);
    lv_obj_remove_style_all(lc);
    lv_obj_set_size(lc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lc, 2, LV_PART_MAIN);
    lv_obj_set_flex_grow(lc, 1);
    lv_obj_clear_flag(lc, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(lc);
    lv_label_set_text(name, TXT_RHYTHM);
    lv_obj_set_style_text_font(name, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    w.rhy_pvc = lv_label_create(lc);
    lv_label_set_text(w.rhy_pvc, "PVC --");
    lv_obj_set_style_text_font(w.rhy_pvc, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.rhy_pvc, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    /* 中：分类字母 */
    w.rhy_letter = lv_label_create(card);
    lv_label_set_text(w.rhy_letter, "--");
    lv_obj_set_style_text_font(w.rhy_letter, &ui_font_Number_big, LV_PART_MAIN);
    lv_obj_set_style_text_color(w.rhy_letter, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    /* 右：状态徽标 */
    w.rhy_badge = make_badge(card);
}

/* ---------- 列表行 ---------- */
static rrow_t build_list_row(lv_obj_t *parent, const char *name,
                             const char *ref_line, int is_last)
{
    rrow_t r;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 54);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (!is_last) {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, UI_COLOR_DIVIDER, LV_PART_MAIN);
        lv_obj_set_style_border_opa(row, LV_OPA_60, LV_PART_MAIN);
    }

    /* 左列：名称 + 参考范围(占据剩余宽度) */
    lv_obj_t *lc = lv_obj_create(row);
    lv_obj_remove_style_all(lc);
    lv_obj_set_size(lc, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lc, 1, LV_PART_MAIN);
    lv_obj_set_flex_grow(lc, 1);
    lv_obj_clear_flag(lc, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = lv_label_create(lc);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_font(nm, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    lv_obj_t *ref = lv_label_create(lc);
    lv_label_set_text(ref, ref_line);
    lv_obj_set_style_text_font(ref, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(ref, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    /* 中：数值(固定宽，右对齐) */
    r.val = lv_label_create(row);
    lv_label_set_text(r.val, "--");
    lv_obj_set_width(r.val, 104);
    lv_obj_set_style_text_align(r.val, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(r.val, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(r.val, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    /* 右：状态徽标 */
    r.badge = make_badge(row);

    return r;
}

/* ---------- 指标列表卡 ---------- */
static void build_list_card(lv_obj_t *parent)
{
    /* section title */
    lv_obj_t *sec = lv_label_create(parent);
    lv_label_set_text(sec, TXT_VITALS);
    lv_obj_set_width(sec, UI_CARD_FULL_W);
    lv_obj_set_style_text_font(sec, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(sec, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(card, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, 4, LV_PART_MAIN);

    w.row[0] = build_list_row(card, TXT_HR,   TXT_REF " 60-100 bpm", 0);
    w.row[1] = build_list_row(card, TXT_SPO2, TXT_REF " 95-100 %", 0);
    w.row[2] = build_list_row(card, TXT_RESP, TXT_REF " 12-20 " TXT_BPM_CN " " TXT_EXP, 0);
    w.row[3] = build_list_row(card, "HRV",    TXT_REF " SDNN 20-120 ms", 0);
    w.row[4] = build_list_row(card, TXT_STRESS, TXT_REF " 0-40", 0);
    w.row[5] = build_list_row(card, TXT_GLU,  TXT_REF " 3.9-6.1 mmol/L " TXT_EXP, 0);
    w.row[6] = build_list_row(card, "PAT",    TXT_REF " " TXT_EXP, 1);
}

/* ---------- 健康建议卡 ---------- */
static void build_advice_card(lv_obj_t *parent)
{
    lv_obj_t *sec = lv_label_create(parent);
    lv_label_set_text(sec, TXT_ADVICE);
    lv_obj_set_width(sec, UI_CARD_FULL_W);
    lv_obj_set_style_text_font(sec, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(sec, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);

    int inner = UI_CARD_FULL_W - 24;
    for (int i = 0; i < HRPT_MAX_ADVICE; i++) {
        lv_obj_t *l = lv_label_create(card);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, inner);
        lv_label_set_text(l, "");
        lv_obj_set_style_text_font(l, &ui_font_Subtitle, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        w.adv[i] = l;
    }

    /* 免责声明(常驻页脚) */
    lv_obj_t *disc = lv_label_create(card);
    lv_label_set_long_mode(disc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(disc, inner);
    lv_label_set_text(disc, HRPT_DISCLAIMER);
    lv_obj_set_style_text_font(disc, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(disc, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
    lv_obj_set_style_pad_top(disc, 4, LV_PART_MAIN);
}

/* ---------- AI 健康建议卡(云端 LLM, Tiny TTF 显示任意 GB2312 汉字) ---------- */
static void ai_advice_refresh_cb(lv_timer_t *t)
{
    (void)t;
    /* 只在 LVGL 线程(定时器上下文)里碰 lv_*; MQTT 线程只置脏 */
    if (!s_ai_dirty || s_ai_lock == RT_NULL || w.ai_advice == NULL)
        return;
    rt_mutex_take(s_ai_lock, RT_WAITING_FOREVER);
    lv_label_set_text(w.ai_advice, s_ai_buf[0] ? s_ai_buf : TXT_AI_WAIT);
    s_ai_dirty = 0;
    rt_mutex_release(s_ai_lock);
}

/*
 * 「开始分析」按键回调：离线立即出设备端版；联网时同时请求云端版。
 *
 * 双路径（2026-07-17 定稿，用户拍板"联网按键要走云端"）：
 *   1. 设备端离线版**无条件先上屏**——保底，任何网络状态都有结果；
 *   2. WiFi+MQTT 都在时再向服务器发 advice_req（自带最新数据快照），
 *      服务器立即调 LLM 生成、经 advice topic 推回，到达后整体覆盖
 *      本卡片（advice_data_cb → ui_report_set_advice）。
 *      尾部提示句让用户知道云端版在路上；若服务器/LLM 挂了，屏上
 *      停留的是完整可用的离线版 + 一句真实的"已请求"，不误导。
 *
 * advice_local_generate 是纯字符串拼接 + 规则判定，无阻塞无 IO；
 * mqtt_advice_request 只 release 信号量 —— 都可在 LVGL 回调直接调。
 */
static void advice_gen_cb(lv_event_t *e)
{
    if (ui_checkup_get()->id) return; /* 云端建议仍绑定实时数据，不能混入本次快照 */
    static char buf[AI_ADVICE_BUF_LEN];
    /* 给云端提示句预留的尾部空间：提示句实测 ~70B(UTF-8)，预留 96。
     * 不预留的话本地正文逼近缓冲上限时提示句会被 rt_snprintf 静默吞掉
     * （对抗审 finding③）。1536-96=1440 仍 > ADVICE_LOCAL_MIN_BUF(1024) */
    #define CLOUD_HINT_RESERVE 96
    (void)e;

    int n = advice_local_generate(buf, sizeof(buf) - CLOUD_HINT_RESERVE);
    if (n <= 0)
        return;

    if (mqtt_advice_ready())
    {
        /* 正文用 tiny_ttf GB2312 全集渲染，中文字面量不受 365 字子集限制 */
        rt_snprintf(buf + n, sizeof(buf) - n,
                    "\n— 已请求云端个性化分析，结果到达后自动更新 —");
        mqtt_advice_request();
    }
    ui_report_set_advice(buf);
}

static void build_ai_advice_card(lv_obj_t *parent)
{
    if (s_ai_lock == RT_NULL)
        s_ai_lock = rt_mutex_create("aiadv", RT_IPC_FLAG_PRIO);

    /* 标题行：左边 "AI 健康建议"，右边「生成建议」按键 */
    lv_obj_t *hdr = lv_obj_create(parent);
    s_ai_header=hdr;
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    /* section title: "AI " + 健康建议(ASCII+子集中文, 用现有静态字体即可) */
    lv_obj_t *sec = lv_label_create(hdr);
    lv_label_set_text(sec, "AI " TXT_ADVICE);
    lv_obj_set_style_text_font(sec, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(sec, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    lv_obj_t *btn = lv_button_create(hdr);
    lv_obj_set_size(btn, 104, 38);
    lv_obj_set_style_bg_color(btn, UI_COLOR_OK, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, advice_gen_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *blbl = lv_label_create(btn);
    /* 开始分析 —— 本 label 用 ui_font_Subtitle(365 字固定子集)，不是 tiny_ttf，
     * 故文案必须过 fonts/check_font_coverage.py。原拟"生成建议"，"成"不在子集里
     * (会渲染成"生□建议")，实测换掉 —— 别信注释，改文案就跑一次脚本 */
    lv_label_set_text(blbl, "\xe5\xbc\x80\xe5\xa7\x8b\xe5\x88\x86\xe6\x9e\x90");
    lv_obj_set_style_text_font(blbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(blbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(blbl);

    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    s_ai_card=card;
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *l = lv_label_create(card);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);   /* 自动换行, 长文本随整页下拉滚动 */
    lv_obj_set_width(l, UI_CARD_FULL_W - 24);
    /* 正文用 Tiny TTF 中文字体: 覆盖 GB2312 全集, 显示云端任意汉字, 字号固定不缩放 */
    lv_font_t *cn = ui_cn_font_get(18);
    lv_obj_set_style_text_font(l, cn ? cn : &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(l, 5, LV_PART_MAIN);
    lv_label_set_text(l, TXT_AI_WAIT);
    w.ai_advice = l;

    /* 独立定时器: 500ms 查脏标志, 有云端新文本才刷(不动现有 refresh_cb) */
    lv_timer_create(ai_advice_refresh_cb, 500, NULL);
}

/* 供外部(MQTT 线程)设置建议正文: 只拷 buffer + 置脏, 不碰 lv_*(跨线程安全) */
void ui_report_set_advice(const char *utf8)
{
    if (s_ai_lock == RT_NULL)
        return;   /* UI 尚未创建, 丢弃(此时通常 MQTT 也未连) */
    rt_mutex_take(s_ai_lock, RT_WAITING_FOREVER);
    if (utf8 && utf8[0]) {
        strncpy(s_ai_buf, utf8, AI_ADVICE_BUF_LEN - 1);
        s_ai_buf[AI_ADVICE_BUF_LEN - 1] = '\0';
    } else {
        s_ai_buf[0] = '\0';
    }
    s_ai_dirty = 1;
    rt_mutex_release(s_ai_lock);
}

/* ====== 从快照填 health_report 输入 ====== */
static void fill_input(const struct ui_health_data *d, health_report_input_t *in)
{
    in->heart_rate      = d->heart_rate;      in->hr_valid       = d->hr_valid;
    in->spo2            = d->spo2;            in->spo2_valid     = d->spo2_valid;
    in->arr_valid       = d->arr_valid && d->arr_pvc_valid; in->arr_class = d->arr_class;
    in->arr_beat_count  = d->arr_beat_count;  in->arr_pvc_per_min= d->arr_pvc_per_min;
    in->hrv_sdnn_ms     = d->hrv_sdnn_ms;     in->hrv_valid      = d->hrv_valid;
    in->resp_rate_bpm   = d->resp_rate_bpm;   in->resp_valid     = d->resp_valid;
    in->gsr_stress      = d->gsr_stress_index;in->gsr_valid      = d->gsr_valid;
    in->glucose_mmol    = d->glucose;         in->glucose_valid  = d->glucose_valid;
    in->health_score    = d->health_score;    in->health_score_valid = d->health_score_valid;
}

/* 按指标给某一列表行填数值字符串(数据不足给 "--") */
static void row_value_str(int row, const struct ui_health_data *d, char *buf, int sz)
{
    char tmp[16];
    switch (row) {
    case 0: /* HR */
        if (d->hr_valid) lv_snprintf(buf, sz, "%d bpm", d->heart_rate);
        else             lv_snprintf(buf, sz, "--");
        break;
    case 1: /* SpO2 */
        if (d->spo2_valid) { ui_fmt_f1(tmp, sizeof(tmp), d->spo2);
                             lv_snprintf(buf, sz, "%s %%", tmp); }
        else               lv_snprintf(buf, sz, "--");
        break;
    case 2: /* 呼吸 */
        if (d->resp_valid) lv_snprintf(buf, sz, "%d " TXT_BPM_CN, (int)(d->resp_rate_bpm + 0.5f));
        else               lv_snprintf(buf, sz, "--");
        break;
    case 3: /* HRV SDNN */
        if (d->hrv_valid) lv_snprintf(buf, sz, "%d ms", (int)(d->hrv_sdnn_ms + 0.5f));
        else              lv_snprintf(buf, sz, "--");
        break;
    case 4: /* 应激 */
        if (d->gsr_valid) lv_snprintf(buf, sz, "%d", (int)(d->gsr_stress_index + 0.5f));
        else              lv_snprintf(buf, sz, "--");
        break;
    case 5: /* 血糖 */
        if (d->glucose_valid) { ui_fmt_f1(tmp, sizeof(tmp), d->glucose);
                                lv_snprintf(buf, sz, "%s mmol/L", tmp); }
        else                  lv_snprintf(buf, sz, "--");
        break;
    case ROW_PAT: /* PAT */
        if (d->pat_valid) lv_snprintf(buf, sz, "%d ms", (int)(d->pat_ms + 0.5f));
        else              lv_snprintf(buf, sz, "--");
        break;
    default: lv_snprintf(buf, sz, "--"); break;
    }
}

/* ====== 周期刷新 ====== */
static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    checkup_widgets_refresh(&s_guide_report);
    if (ui_checkup_get()->id) {
        lv_obj_add_flag(s_ai_header,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ai_card,LV_OBJ_FLAG_HIDDEN);
    }
    struct ui_health_data snapshot;
    ui_checkup_report(&snapshot);
    const struct ui_health_data *d = &snapshot;
    health_report_input_t in;
    health_report_t r;
    char buf[128];   /* 需容纳最长建议句(~70 字节 UTF-8)，太小会截断多字节字符 */
    int i;

    fill_input(d, &in);
    health_report_build(&in, &r);

    /* 综合评估 */
    lv_arc_set_value(w.score_arc, r.score_valid ? r.score : 0);
    if (r.score_valid) lv_label_set_text_fmt(w.score_num, "%d", r.score);
    else               lv_label_set_text(w.score_num, "--");
    lv_obj_set_style_text_color(w.score_num, level_color(r.grade_level), LV_PART_MAIN);
    lv_obj_set_style_arc_color(w.score_arc, level_color(r.grade_level), LV_PART_INDICATOR);

    lv_label_set_text(w.grade, r.grade_text);
    lv_obj_set_style_text_color(w.grade, level_color(r.grade_level), LV_PART_MAIN);

    const checkup_flow_t *flow=ui_checkup_get();
    lv_obj_set_style_bg_color(w.status_dot,
        (flow->id ? flow->state==CHECKUP_DONE && flow->completed==3 : d->sensor_link_valid)?UI_COLOR_OK:UI_COLOR_WARN,0);

    lv_snprintf(buf, sizeof(buf), "%d " HRPT_SUM_NORMAL "  %d " HRPT_SUM_WATCH,
                r.normal_count, r.watch_count);
    lv_label_set_text(w.summary, buf);
    if (flow->id) {
        lv_font_t *cn=ui_cn_font_get(18);
        lv_obj_set_style_text_font(w.grade,cn?cn:&ui_font_Subtitle,0);
        lv_obj_set_style_text_font(w.summary,cn?cn:&ui_font_Subtitle,0);
        /* 完成流程不等于所有项目正常，跳过项目后也不继续显示“评估中”。 */
        if (flow->state!=CHECKUP_DONE || flow->completed!=3)
            lv_label_set_text(w.grade,cn?(flow->state==CHECKUP_CANCELLED?"体检已取消":
                flow->state==CHECKUP_DONE?"项目不全":"正在体检"):"Incomplete");
        else lv_label_set_text(w.grade,cn?"规则评分":"Rule score");
        lv_snprintf(buf,sizeof(buf),cn?"已完成 %u/2 组":"Completed %u/2 groups",
            (unsigned)((flow->completed&1)!=0)+(unsigned)((flow->completed&2)!=0));
        lv_label_set_text(w.summary,buf);
    }

    /* 心律旗舰卡 */
    {
        const hrpt_eval_t *e = &r.m[HRPT_M_RHYTHM];
        if (d->arr_valid) lv_label_set_text(w.rhy_letter, arr_class_letter(d->arr_class));
        else              lv_label_set_text(w.rhy_letter, "--");
        lv_obj_set_style_text_color(w.rhy_letter, level_color(e->level), LV_PART_MAIN);
        set_badge(w.rhy_badge, e->status_text, level_color(e->level));
        if (d->arr_pvc_valid) lv_snprintf(buf, sizeof(buf), "PVC %u " TXT_BPM_CN, d->arr_pvc_per_min);
        else              lv_snprintf(buf, sizeof(buf), "PVC --");
        lv_label_set_text(w.rhy_pvc, buf);
    }

    /* 指标列表 */
    for (i = 0; i < REPORT_ROWS; i++) {
        row_value_str(i, d, buf, sizeof(buf));
        lv_label_set_text(w.row[i].val, buf);

        int metric = ROW_METRIC[i];
        if (metric < 0) {
            /* PAT：无解读，仅标实验性/未测 */
            if (d->pat_valid) set_badge(w.row[i].badge, TXT_EXP, UI_COLOR_TEXT_SEC);
            else              set_badge(w.row[i].badge, "--", UI_COLOR_TEXT_SEC);
        } else {
            const hrpt_eval_t *e = &r.m[metric];
            set_badge(w.row[i].badge, e->status_text, level_color(e->level));
        }
    }

    /* 健康建议 */
    for (i = 0; i < HRPT_MAX_ADVICE; i++) {
        if (i < r.advice_count) {
            lv_snprintf(buf, sizeof(buf), "- %s", r.advice[i]);
            lv_label_set_text(w.adv[i], buf);
            lv_obj_clear_flag(w.adv[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(w.adv[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ====== 入口 ====== */
void ui_tab_report_create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(parent, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(parent, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, UI_PAD_GAP, LV_PART_MAIN);
    /* 内容高于一屏 → 纵向滚动 */
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);

    build_status_bar(parent);
    checkup_widgets_create(&s_guide_report,parent,0);
    build_assess_hero(parent);
    build_rhythm_card(parent);
    build_list_card(parent);
    build_advice_card(parent);
    build_ai_advice_card(parent);

    lv_timer_create(refresh_cb, 1000, NULL);
}
