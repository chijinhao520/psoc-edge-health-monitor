/**
 * @file  ui_wifi_panel.c
 * @brief WiFi 扫描列表全屏覆盖层
 *
 * 点击设置页 WiFi 行后弹出，显示扫描到的 AP 列表。
 * 扫描在后台线程执行（~10s），UI 通过 LVGL 定时器轮询状态。
 */
#include "ui_wifi_panel.h"
#include "ui_common.h"
#include "../wifi_connect.h"

#include <string.h>
#include <stdint.h>

/* ---- 布局常量 ---- */
#define TOP_BAR_H       56
#define ITEM_H          52
#define ITEM_PAD_VER    16   /* 加大列表项上下留白，配合 min_height 更好点按 */

/* ---- 模块状态 ---- */
static lv_obj_t  *_panel     = NULL;   /* 全屏覆盖层容器 */
static lv_obj_t  *_list      = NULL;   /* AP 列表滚动容器 */
static lv_obj_t  *_spinner   = NULL;   /* 扫描中动画 */
static lv_obj_t  *_status_lbl = NULL;  /* "正在扫描..." / "扫描完成" */
static lv_timer_t *_poll_tmr = NULL;   /* 轮询扫描状态的定时器 */

/* ---- 列表项 SSID/密码需求缓存(按下标索引，供点击回调取用) ----
 * wifi_scan_get_results() 拷贝进 populate_list() 的是栈上临时数组，
 * 点击回调触发时早就出栈了，必须自己另存一份长期有效的副本。 */
static char    _item_ssid[WIFI_SCAN_MAX][WIFI_SSID_MAX_LEN];
static uint8_t _item_has_pwd[WIFI_SCAN_MAX];

/* ---- 密码输入弹层 ---- */
static lv_obj_t *_pwd_dialog   = NULL;
static lv_obj_t *_pwd_textarea = NULL;
static lv_obj_t *_pwd_keyboard = NULL;

/* ---- 连接状态提示 + 轮询 ---- */
static lv_obj_t   *_conn_status_lbl = NULL;
static lv_timer_t *_conn_poll_tmr   = NULL;

/* ---- 密码键盘自定义大键位布局 ----
 * 默认 lv_keyboard 每行 10~11 键，在 413px 宽的弹层里每键只有 ~40px，
 * 演示时容易按错相邻键。改成每行最多 6 键（字母行按原 QWERTY 顺序
 * 拆成两行，视觉扫读顺序不变），键宽涨到 ~68px。数字直接铺在同一页
 * 而不是走"1#"切到 SPECIAL 模式，省掉再定义一套数字页的工作量。
 * "ABC"/"abc" 沿用 LVGL 内置大小写切换识别字符串，lv_keyboard_def_event_cb
 * 会自动处理，不用自己写切换逻辑。 */
static const char *_pwd_kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "\n",
    "u", "i", "o", "p", "a", "s", "\n",
    "d", "f", "g", "h", "j", "k", "\n",
    "l", "z", "x", "c", "v", "b", "\n",
    "n", "m", "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", "7", "8", "9", "\n",
    "ABC", "0", " ", ""
};
static const lv_buttonmatrix_ctrl_t _pwd_kb_ctrl_lc[] = {
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, LV_BUTTONMATRIX_CTRL_CHECKED | 1,
    1, 1, 1, 1, 1, 1,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2, 1, 4
};
static const char *_pwd_kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "\n",
    "U", "I", "O", "P", "A", "S", "\n",
    "D", "F", "G", "H", "J", "K", "\n",
    "L", "Z", "X", "C", "V", "B", "\n",
    "N", "M", "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", "7", "8", "9", "\n",
    "abc", "0", " ", ""
};
static const lv_buttonmatrix_ctrl_t _pwd_kb_ctrl_uc[] = {
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, LV_BUTTONMATRIX_CTRL_CHECKED | 1,
    1, 1, 1, 1, 1, 1,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2, 1, 4
};

/* ---- 前向声明 ---- */
static void build_top_bar(lv_obj_t *parent);
static void build_list_area(lv_obj_t *parent);
static void poll_scan_cb(lv_timer_t *tmr);
static void populate_list(void);
static void back_btn_cb(lv_event_t *e);
static void ap_item_click_cb(lv_event_t *e);
static void show_password_dialog(int idx);
static void hide_password_dialog(void);
static void start_connect(const char *ssid, const char *password);

/* ================================================================
 *  公开接口
 * ================================================================ */

void ui_wifi_panel_show(void)
{
    if (_panel != NULL)
        return;  /* 已打开 */

    /* 全屏覆盖层，创建在 active screen 上 */
    _panel = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(_panel);
    lv_obj_set_size(_panel, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(_panel, 0, 0);
    lv_obj_set_style_bg_color(_panel, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 内容区（补偿 VRAM 左偏移） */
    lv_obj_set_style_pad_left(_panel, UI_PAD_SIDE + UI_VRAM_LEFT_MARGIN, LV_PART_MAIN);
    lv_obj_set_style_pad_right(_panel, UI_PAD_SIDE, LV_PART_MAIN);
    lv_obj_set_style_pad_top(_panel, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    build_top_bar(_panel);
    build_list_area(_panel);

    /* 将覆盖层置于最前（但 VRAM guard 之前—guard 由 ui_main 在 scr 最上层管理） */
    lv_obj_move_foreground(_panel);

    /* 触发扫描 */
    wifi_start_scan();

    /* 启动轮询定时器（200ms 周期） */
    _poll_tmr = lv_timer_create(poll_scan_cb, 200, NULL);
}

void ui_wifi_panel_hide(void)
{
    if (_poll_tmr != NULL)
    {
        lv_timer_delete(_poll_tmr);
        _poll_tmr = NULL;
    }
    if (_conn_poll_tmr != NULL)
    {
        lv_timer_delete(_conn_poll_tmr);
        _conn_poll_tmr = NULL;
    }

    /* _pwd_dialog 是独立顶层对象(平级于 _panel，不是子对象)，
     * 必须单独关闭，_panel 删除不会连带删它 */
    hide_password_dialog();

    if (_panel != NULL)
    {
        lv_obj_delete(_panel);
        _panel = NULL;
        _list = NULL;
        _spinner = NULL;
        _status_lbl = NULL;
        _conn_status_lbl = NULL;
    }
}

/* ================================================================
 *  顶栏
 * ================================================================ */

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_panel_hide();
}

static void build_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), TOP_BAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(bar, 4, LV_PART_MAIN);

    /* 返回按钮 */
    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 70, 36);
    lv_obj_set_style_bg_color(back, UI_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(back, UI_COLOR_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back);
    /* "< 返回" */
    lv_label_set_text(back_lbl, "< \xe8\xbf\x94\xe5\x9b\x9e");
    lv_obj_set_style_text_font(back_lbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_lbl, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    lv_obj_center(back_lbl);

    /* 标题 */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "WiFi");
    lv_obj_set_style_text_font(title, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

/* ================================================================
 *  列表区域
 * ================================================================ */

static void build_list_area(lv_obj_t *parent)
{
    /* 可滚动容器 */
    _list = lv_obj_create(parent);
    lv_obj_remove_style_all(_list);
    lv_obj_set_width(_list, lv_pct(100));
    lv_obj_set_flex_grow(_list, 1);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(_list, 0, LV_PART_MAIN);
    lv_obj_add_flag(_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);

    /* 扫描中提示 */
    lv_obj_t *hint_row = lv_obj_create(_list);
    lv_obj_remove_style_all(hint_row);
    lv_obj_set_size(hint_row, lv_pct(100), 60);
    lv_obj_set_flex_flow(hint_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hint_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(hint_row, 12, LV_PART_MAIN);

    _spinner = lv_spinner_create(hint_row);
    lv_obj_set_size(_spinner, 28, 28);
    lv_spinner_set_anim_params(_spinner, 1000, 270);

    /* "正在扫描..." */
    _status_lbl = lv_label_create(hint_row);
    lv_label_set_text(_status_lbl,
        "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f...");
    lv_obj_set_style_text_font(_status_lbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(_status_lbl, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
}

/* ================================================================
 *  扫描结果轮询 + 列表构建
 * ================================================================ */

/* RSSI → 信号颜色 */
static lv_color_t rssi_color(int16_t rssi)
{
    if (rssi > -50)  return UI_COLOR_OK;     /* 强 */
    if (rssi > -70)  return UI_COLOR_WARN;   /* 中 */
    return UI_COLOR_DANGER;                   /* 弱 */
}

/* RSSI → 信号文字 */
static const char *rssi_icon(int16_t rssi)
{
    if (rssi > -50)  return "|||";
    if (rssi > -70)  return "|| ";
    return "|  ";
}

/* SSID 里有没有非 ASCII 字节(0x80+)——超出我们字体覆盖范围的外部字符串，
 * 硬渲染只会变成一串口口，不如显示统一占位文字更干净 */
static rt_bool_t ssid_has_non_ascii(const char *ssid)
{
    for (const unsigned char *p = (const unsigned char *)ssid; *p; p++)
    {
        if (*p >= 0x80)
            return RT_TRUE;
    }
    return RT_FALSE;
}

/* 创建一个 AP 列表项(idx 对应 _item_ssid/_item_has_pwd 里存的副本下标) */
static void build_ap_item(lv_obj_t *parent, const wifi_scan_item_t *ap, int idx)
{
    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_min_height(card, 68, LV_PART_MAIN);  /* 加大触摸区域，方便点按 */
    lv_obj_set_style_pad_ver(card, ITEM_PAD_VER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 12, LV_PART_MAIN);

    /* 整卡可点击 + 按下反馈，点击后弹密码框/直接连开放网络 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xE2E8F0), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_50, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, ap_item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    /* 左侧：锁 + SSID */
    lv_obj_t *left = lv_obj_create(card);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(left, 6, LV_PART_MAIN);
    lv_obj_set_flex_grow(left, 1);

    lv_obj_t *ssid_lbl = lv_label_create(left);
    if (ssid_has_non_ascii(ap->ssid))
    {
        /* 字体没覆盖到的外部字符——与其显示一串口口，不如给个干净占位 */
        lv_label_set_text(ssid_lbl, "\xe7\xbd\x91\xe7\xbb\x9c");  /* 网络 */
    }
    else
    {
        lv_label_set_text(ssid_lbl, ap->ssid);
    }
    lv_obj_set_style_text_font(ssid_lbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(ssid_lbl, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ssid_lbl, 220);

    /* 右侧：密码标识 + 信号强度 + dBm */
    lv_obj_t *right = lv_obj_create(card);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(right, 8, LV_PART_MAIN);

    if (ap->has_password)
    {
        /* LVGL 内置符号里没有锁图标，EYE_CLOSE 语义不够直接，
         * 干脆用文字"密码"，比图标更不容易让人误解 */
        lv_obj_t *pwd_tag = lv_label_create(right);
        lv_label_set_text(pwd_tag, "\xe5\xaf\x86\xe7\xa0\x81");  /* 密码 */
        lv_obj_set_style_text_font(pwd_tag, &ui_font_Subtitle, LV_PART_MAIN);
        lv_obj_set_style_text_color(pwd_tag, UI_COLOR_WARN, LV_PART_MAIN);
    }

    lv_obj_t *sig = lv_label_create(right);
    lv_label_set_text(sig, rssi_icon(ap->rssi));
    lv_obj_set_style_text_color(sig, rssi_color(ap->rssi), LV_PART_MAIN);
    lv_obj_set_style_text_font(sig, &ui_font_Subtitle, LV_PART_MAIN);

    char dbm[8];
    lv_snprintf(dbm, sizeof(dbm), "%d", (int)ap->rssi);
    lv_obj_t *dbm_lbl = lv_label_create(right);
    lv_label_set_text(dbm_lbl, dbm);
    lv_obj_set_style_text_font(dbm_lbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(dbm_lbl, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
}

/* 构建当前连接卡片 */
static void build_connected_card(lv_obj_t *parent)
{
    const char *ssid = wifi_get_connected_ssid();
    const char *ip   = wifi_get_ip_str();
    if (ssid == NULL)
        return;

    lv_obj_t *card = ui_card_create(parent, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);

    /* 左侧绿色边框标识 */
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, UI_COLOR_OK, LV_PART_MAIN);

    /* "已连接" + SSID */
    char buf[64];
    /* "已连接: xxx" */
    lv_snprintf(buf, sizeof(buf),
        "\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5: %s", ssid);
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, UI_COLOR_OK, LV_PART_MAIN);

    if (ip && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0)
    {
        lv_snprintf(buf, sizeof(buf), "IP: %s", ip);
        lv_obj_t *ip_lbl = lv_label_create(card);
        lv_label_set_text(ip_lbl, buf);
        lv_obj_set_style_text_font(ip_lbl, &ui_font_Subtitle, LV_PART_MAIN);
        lv_obj_set_style_text_color(ip_lbl, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
    }
}

/* 填充列表 */
static void populate_list(void)
{
    if (_list == NULL)
        return;

    /* 清空列表(会连带删掉 _conn_status_lbl 这个子对象，先置空防悬空指针) */
    _conn_status_lbl = NULL;
    lv_obj_clean(_list);

    /* 当前连接 */
    build_connected_card(_list);

    /* 扫描结果 */
    wifi_scan_item_t items[WIFI_SCAN_MAX];
    int count = wifi_scan_get_results(items, WIFI_SCAN_MAX);

    if (count == 0)
    {
        lv_obj_t *empty = lv_label_create(_list);
        /* "未找到WiFi" */
        lv_label_set_text(empty,
            "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0 WiFi");
        lv_obj_set_style_text_font(empty, &ui_font_Subtitle, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
        lv_obj_set_style_pad_top(empty, 20, LV_PART_MAIN);
        return;
    }

    /* 扫描结果标题 */
    lv_obj_t *hdr = lv_label_create(_list);
    char hdr_buf[32];
    /* "可用网络 (N)" */
    lv_snprintf(hdr_buf, sizeof(hdr_buf),
        "\xe5\x8f\xaf\xe7\x94\xa8\xe7\xbd\x91\xe7\xbb\x9c (%d)", count);
    lv_label_set_text(hdr, hdr_buf);
    lv_obj_set_style_text_font(hdr, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(hdr, UI_COLOR_TEXT_PRI, LV_PART_MAIN);
    lv_obj_set_style_pad_top(hdr, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(hdr, 4, LV_PART_MAIN);

    for (int i = 0; i < count && i < WIFI_SCAN_MAX; i++)
    {
        rt_strncpy(_item_ssid[i], items[i].ssid, WIFI_SSID_MAX_LEN - 1);
        _item_ssid[i][WIFI_SSID_MAX_LEN - 1] = '\0';
        _item_has_pwd[i] = items[i].has_password;

        build_ap_item(_list, &items[i], i);
    }
}

/* 定时器回调：轮询扫描状态 */
static void poll_scan_cb(lv_timer_t *tmr)
{
    (void)tmr;

    int status = wifi_scan_status();

    if (status == WIFI_SCAN_DONE || status == WIFI_SCAN_FAILED)
    {
        /* 停止定时器 */
        if (_poll_tmr)
        {
            lv_timer_delete(_poll_tmr);
            _poll_tmr = NULL;
        }

        if (status == WIFI_SCAN_FAILED && _status_lbl)
        {
            /* "扫描失败" */
            lv_label_set_text(_status_lbl,
                "\xe6\x89\xab\xe6\x8f\x8f\xe5\xa4\xb1\xe8\xb4\xa5");
            if (_spinner)
                lv_obj_add_flag(_spinner, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        /* 扫描成功，重建列表 */
        populate_list();
    }
}

/* ================================================================
 *  连接状态轮询
 * ================================================================ */

static void connect_poll_cb(lv_timer_t *tmr)
{
    (void)tmr;
    int status = wifi_manual_connect_status();

    if (status == WIFI_CONN_CONNECTING)
        return;  /* 还在连，继续等 */

    if (_conn_poll_tmr)
    {
        lv_timer_delete(_conn_poll_tmr);
        _conn_poll_tmr = NULL;
    }

    if (status == WIFI_CONN_SUCCESS)
    {
        /* 直接重建列表——顶部会出现"已连接"卡片，比单独维护一条状态文字更直观 */
        populate_list();
        _conn_status_lbl = NULL;  /* populate_list 内部 lv_obj_clean 已经把旧 label 删掉 */
    }
    else if (_conn_status_lbl != NULL)
    {
        /* "连接失败" */
        lv_label_set_text(_conn_status_lbl,
            "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\xa4\xb1\xe8\xb4\xa5");
        lv_obj_set_style_text_color(_conn_status_lbl, UI_COLOR_DANGER, LV_PART_MAIN);
    }
}

static void start_connect(const char *ssid, const char *password)
{
    wifi_connect_to(ssid, password);

    if (_list != NULL)
    {
        _conn_status_lbl = lv_label_create(_list);
        lv_obj_move_to_index(_conn_status_lbl, 0);
        lv_obj_set_style_text_font(_conn_status_lbl, &ui_font_Subtitle, LV_PART_MAIN);
        /* "正在连接..." */
        lv_label_set_text(_conn_status_lbl,
            "\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5...");
        lv_obj_set_style_text_color(_conn_status_lbl, UI_COLOR_TEXT_SEC, LV_PART_MAIN);
    }

    if (_conn_poll_tmr == NULL)
        _conn_poll_tmr = lv_timer_create(connect_poll_cb, 500, NULL);
}

/* ================================================================
 *  密码输入弹层
 * ================================================================ */

static int _pending_idx = -1;

static void pwd_cancel_cb(lv_event_t *e)
{
    (void)e;
    hide_password_dialog();
}

static void pwd_connect_cb(lv_event_t *e)
{
    (void)e;
    if (_pending_idx < 0 || _pending_idx >= WIFI_SCAN_MAX)
    {
        hide_password_dialog();
        return;
    }

    const char *pwd = _pwd_textarea ? lv_textarea_get_text(_pwd_textarea) : "";
    char ssid_copy[WIFI_SSID_MAX_LEN];
    rt_strncpy(ssid_copy, _item_ssid[_pending_idx], WIFI_SSID_MAX_LEN - 1);
    ssid_copy[WIFI_SSID_MAX_LEN - 1] = '\0';

    char pwd_copy[65];
    rt_strncpy(pwd_copy, pwd, sizeof(pwd_copy) - 1);
    pwd_copy[sizeof(pwd_copy) - 1] = '\0';

    hide_password_dialog();
    start_connect(ssid_copy, pwd_copy);
}

static void hide_password_dialog(void)
{
    if (_pwd_dialog != NULL)
    {
        lv_obj_delete(_pwd_dialog);
        _pwd_dialog   = NULL;
        _pwd_textarea = NULL;
        _pwd_keyboard = NULL;
    }
}

static void show_password_dialog(int idx)
{
    if (_pwd_dialog != NULL || idx < 0 || idx >= WIFI_SCAN_MAX)
        return;

    _pending_idx = idx;

    /* 建在 lv_screen_active() 上，跟 _panel 是平级的独立顶层覆盖层——
     * 不能挂在 _panel 下面：_panel 是 flex 布局容器且自己已经加了一层
     * UI_VRAM_LEFT_MARGIN padding，嵌套进去会导致 padding 叠加 + flex
     * 子节点不支持绝对定位，渲染位置和触摸坐标对不上（实测复现过）。 */
    _pwd_dialog = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(_pwd_dialog);
    lv_obj_set_size(_pwd_dialog, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(_pwd_dialog, 0, 0);
    lv_obj_set_style_bg_color(_pwd_dialog, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_pwd_dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(_pwd_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_left(_pwd_dialog, UI_PAD_SIDE + UI_VRAM_LEFT_MARGIN, LV_PART_MAIN);
    lv_obj_set_style_pad_right(_pwd_dialog, UI_PAD_SIDE, LV_PART_MAIN);
    lv_obj_set_style_pad_top(_pwd_dialog, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(_pwd_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_pwd_dialog, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(_pwd_dialog, 12, LV_PART_MAIN);
    lv_obj_move_foreground(_pwd_dialog);

    /* "连接到 <SSID>" */
    char buf[64];
    lv_snprintf(buf, sizeof(buf), "\xe8\xbf\x9e\xe6\x8e\xa5\xe5\x88\xb0 %s", _item_ssid[idx]);
    lv_obj_t *title = lv_label_create(_pwd_dialog);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, &ui_font_Title, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_PRI, LV_PART_MAIN);

    /* "密码" */
    lv_obj_t *pwd_lbl = lv_label_create(_pwd_dialog);
    lv_label_set_text(pwd_lbl, "\xe5\xaf\x86\xe7\xa0\x81");
    lv_obj_set_style_text_font(pwd_lbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_set_style_text_color(pwd_lbl, UI_COLOR_TEXT_SEC, LV_PART_MAIN);

    _pwd_textarea = lv_textarea_create(_pwd_dialog);
    lv_obj_set_size(_pwd_textarea, UI_CARD_FULL_W, 44);
    lv_textarea_set_one_line(_pwd_textarea, true);
    lv_textarea_set_password_mode(_pwd_textarea, true);
    lv_textarea_set_max_length(_pwd_textarea, 63);

    lv_obj_t *btn_row = lv_obj_create(_pwd_dialog);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, UI_CARD_FULL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, UI_PAD_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_set_size(cancel_btn, 100, 40);
    lv_obj_add_event_cb(cancel_btn, pwd_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "\xe8\xbf\x94\xe5\x9b\x9e");  /* 返回 */
    lv_obj_set_style_text_font(cancel_lbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    lv_obj_t *conn_btn = lv_button_create(btn_row);
    lv_obj_set_size(conn_btn, 100, 40);
    lv_obj_set_style_bg_color(conn_btn, UI_COLOR_GLUCOSE, LV_PART_MAIN);
    lv_obj_add_event_cb(conn_btn, pwd_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *conn_lbl = lv_label_create(conn_btn);
    lv_label_set_text(conn_lbl, "\xe8\xbf\x9e\xe6\x8e\xa5");  /* 连接 */
    lv_obj_set_style_text_font(conn_lbl, &ui_font_Subtitle, LV_PART_MAIN);
    lv_obj_center(conn_lbl);

    _pwd_keyboard = lv_keyboard_create(_pwd_dialog);
    lv_keyboard_set_textarea(_pwd_keyboard, _pwd_textarea);
    /* 宽度必须用 UI_CARD_FULL_W（跟同一弹层里的输入框/按钮行一致），
     * 之前误用 UI_SCREEN_W(512,含左侧黑色导轨) 导致键盘比弹层内容区
     * (413px)宽 67px，flex 居中布局下两侧各溢出~33px——左边溢出到
     * UI_VRAM_LEFT_MARGIN 黑色导轨下面、右边溢出到物理屏幕右边缘外，
     * 这才是"键盘错位"的真正原因(跟之前修的父子嵌套 padding 叠加是
     * 两个独立的 bug)。改用每行 6 键的自定义大键位布局后行数变多
     * (7 行)，高度从 320 加到 440 保持每行~63px 的舒适按键高度。 */
    lv_keyboard_set_map(_pwd_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, _pwd_kb_map_lc, _pwd_kb_ctrl_lc);
    lv_keyboard_set_map(_pwd_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, _pwd_kb_map_uc, _pwd_kb_ctrl_uc);
    lv_obj_set_size(_pwd_keyboard, UI_CARD_FULL_W, 440);
}

/* ================================================================
 *  AP 列表项点击
 * ================================================================ */

static void ap_item_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= WIFI_SCAN_MAX)
        return;

    if (_item_has_pwd[idx])
        show_password_dialog(idx);
    else
        start_connect(_item_ssid[idx], "");
}
