/**
 * @file  ui_wifi_panel.h
 * @brief WiFi 扫描列表全屏覆盖层
 */
#ifndef __UI_WIFI_PANEL_H__
#define __UI_WIFI_PANEL_H__

#include "lvgl.h"

/** 显示 WiFi 扫描列表覆盖层（自动触发扫描） */
void ui_wifi_panel_show(void);

/** 关闭覆盖层 */
void ui_wifi_panel_hide(void);

#endif /* __UI_WIFI_PANEL_H__ */
