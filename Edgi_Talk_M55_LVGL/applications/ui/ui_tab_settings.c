/**
 * @file  ui_tab_settings.c
 * @brief Tab 3 — device info, connection status, measurement config.
 */
#include "ui_tab_settings.h"
#include "ui_common.h"
#include "ui_wifi_panel.h"
#include "../wifi_connect.h"
#include "../mqtt_upload.h"
#include "ui_data.h"

static struct {
    lv_obj_t *wifi_status;
    lv_obj_t *upload_status;
    lv_obj_t *sensor_link;
    lv_obj_t *ppg_status;
    lv_obj_t *temp_status;
    lv_obj_t *env_status;
    lv_obj_t *gsr_status;
    lv_obj_t *ecg_status;
    lv_obj_t *hrv_status;
} w;

/* ---- helpers ---- */

/* Section header label */
static void build_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    lv_obj_set_style_pad_top(lbl, 8, LV_PART_MAIN);
}

/* Key-value info row inside a card */
static lv_obj_t *build_info_row(lv_obj_t *card, const char *key, const char *val)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_ver(row, 6, LV_PART_MAIN);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_font(k, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(k, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_obj_set_style_text_font(v, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    return v;
}

/* Divider line inside card */
static void build_divider(lv_obj_t *card)
{
    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line, UI_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
}

/* ---- build sections ---- */

static void build_device_info(lv_obj_t *parent)
{
    build_section(parent,
        "\xe8\xae\xbe\xe5\xa4\x87\xe4\xbf\xa1\xe6\x81\xaf");  /* 设备信息 */

    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN);

    build_info_row(card,
        "\xe8\xae\xbe\xe5\xa4\x87\xe5\x90\x8d\xe7\xa7\xb0",   /* 设备名称 */
        "PSoC Edge E84");
    build_divider(card);
    build_info_row(card,
        "\xe4\xb8\xbb\xe6\x8e\xa7",                             /* 主控 */
        "M55@400M + M33@200M");
    build_divider(card);
    build_info_row(card,
        "NPU",
        "Ethos-U55 0.1TOPS");
    build_divider(card);
    build_info_row(card,
        "\xe5\x9b\xba\xe4\xbb\xb6\xe7\x89\x88\xe6\x9c\xac",   /* 固件版本 */
        "v0.1.0-alpha");
}

/* WiFi 行点击回调 */
static void wifi_row_click_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_panel_show();
}

/* Build a clickable WiFi row with ">" arrow */
static void build_wifi_row(lv_obj_t *card)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 48);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* 整行可点击 + 按下时变色反馈 */
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xE2E8F0), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_50, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, wifi_row_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, "Wi-Fi");
    lv_obj_set_style_text_font(k, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(k, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
    /* label 不拦截触摸，事件穿透给 row */
    lv_obj_add_flag(k, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);

    /* 右侧：状态 + ">" 箭头（直接放 label，不套额外容器） */
    lv_obj_t *v = lv_label_create(row);
    /* "未连接 >"，实际状态由 refresh_wifi_status_cb 定时刷新 */
    lv_label_set_text(v,
        "\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5 >");
    lv_obj_set_style_text_font(v, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    lv_obj_add_flag(v, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    w.wifi_status = v;
}

/* 定时刷新 WiFi 行状态文字(1s 一次，跟 monitor tab 同频率) */
static void refresh_wifi_status_cb(lv_timer_t *t)
{
    (void)t;
    if (w.wifi_status == NULL)
        return;

    if (wifi_is_connected())
    {
        char buf[48];
        const char *ssid = wifi_get_connected_ssid();
        lv_snprintf(buf, sizeof(buf), "%s >", ssid ? ssid : "");
        lv_label_set_text(w.wifi_status, buf);
    }
    else
    {
        lv_label_set_text(w.wifi_status,
            "\xe6\x9c\xaa\xe8\xbf\x9e\xe6\x8e\xa5 >");  /* 未连接 > */
    }
    const struct ui_health_data *d=ui_data_get();
    lv_label_set_text(w.upload_status,mqtt_advice_ready() ? "已连接" : "未连接");
    lv_label_set_text(w.sensor_link,d->sensor_link_valid ? "已连接" : "未连接");
    lv_label_set_text(w.ppg_status,d->hr_valid && d->spo2_valid ? "数据有效" : "未测或失效");
    lv_label_set_text(w.temp_status,d->body_temp_valid ? "数据有效" : "未测或失效");
    lv_label_set_text(w.env_status,d->env_valid ? "数据有效" : "未测或失效");
    lv_label_set_text(w.gsr_status,d->gsr_valid ? "数据有效" : "未测或失效");
    lv_label_set_text(w.ecg_status,d->arr_valid ? "数据有效" : "未测或失效");
    lv_label_set_text(w.hrv_status,d->hrv_valid ? "数据有效" : "未测或失效");
}

static void build_connection(lv_obj_t *parent)
{
    build_section(parent,
        "\xe8\xbf\x9e\xe6\x8e\xa5\xe7\x8a\xb6\xe6\x80\x81");  /* 连接状态 */

    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN);

    build_wifi_row(card);  /* 可点击的 WiFi 行 */
    build_divider(card);
    w.upload_status=build_info_row(card,"数据上传","未连接");
}

static void build_sensor_status(lv_obj_t *parent)
{
    build_section(parent,"检测状态");
    lv_obj_t *card=ui_card_create(parent,UI_CARD_FULL_W,LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card,0,LV_PART_MAIN);
    w.sensor_link=build_info_row(card,"采集端","未连接");
    w.ppg_status=build_info_row(card,"心率/血氧","未测或失效");
    w.temp_status=build_info_row(card,"红外温度","未测或失效");
    w.env_status=build_info_row(card,"环境温湿度","未测或失效");
    w.gsr_status=build_info_row(card,"GSR","未测或失效");
    w.ecg_status=build_info_row(card,"心律分析","未测或失效");
    w.hrv_status=build_info_row(card,"HRV","未测或失效");
}

static void build_measure_config(lv_obj_t *parent)
{
    build_section(parent,
        "\xe9\x87\x87\xe6\xa0\xb7\xe9\x85\x8d\xe7\xbd\xae");  /* 采样配置 */

    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN);

    build_info_row(card,
        "PPG\xe9\x87\x87\xe6\xa0\xb7",                          /* PPG采样 */
        "100 Hz");
    build_divider(card);
    /* 一般需贴合确认5秒、稳定5秒及3窗聚合15秒；失败或等待最多90秒。 */
    build_info_row(card,
        "\xe6\xb5\x8b\xe9\x87\x8f\xe7\x94\xa8\xe6\x97\xb6",   /* 测量用时 */
        "25-90\xe7\xa7\x92");  /* 含贴合确认、稳定与聚合；90秒总上限 */
    build_divider(card);
    /* 心律+血糖双分支 CNN，经 Vela 编译跑 Ethos-U55 —— 别写代码里
     * 不存在的模型名（此处曾写 "Ridge Regression"，全仓无该实现） */
    build_info_row(card,
        "\xe6\x8e\xa8\xe7\x90\x86\xe6\xa8\xa1\xe5\x9e\x8b",   /* 推理模型 */
        "CNN (Ethos-U55)");
}

static void build_about(lv_obj_t *parent)
{
    build_section(parent,
        "\xe5\x85\xb3\xe4\xba\x8e");  /* 关于 */

    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN);

    build_info_row(card,
        "\xe8\xb5\x9b\xe9\xa2\x98",                             /* 赛题 */
        /* 嵌入式人工智能 */
        "\xe5\xb5\x8c\xe5\x85\xa5\xe5\xbc\x8f\xe4\xba\xba\xe5\xb7\xa5\xe6\x99\xba\xe8\x83\xbd");
    build_divider(card);
    build_info_row(card,
        "\xe8\x8a\xaf\xe7\x89\x87",                             /* 芯片 */
        /* 英飞凌 PSoC Edge E84 */
        "\xe8\x8b\xb1\xe9\xa3\x9e\xe5\x87\x8c PSoC E84");
    build_divider(card);
    build_info_row(card,
        "\xe7\xb3\xbb\xe7\xbb\x9f",                             /* 系统 */
        "RT-Thread + LVGL 9.2");
}

/* ---- public ---- */

void ui_tab_settings_create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(parent, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(parent, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 6, LV_PART_MAIN);

    build_device_info(parent);
    build_connection(parent);
    build_sensor_status(parent);
    build_measure_config(parent);
    build_about(parent);

    lv_timer_create(refresh_wifi_status_cb, 1000, NULL);
}
