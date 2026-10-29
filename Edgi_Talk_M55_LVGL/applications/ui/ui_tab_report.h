#ifndef __UI_TAB_REPORT_H__
#define __UI_TAB_REPORT_H__

#include "lvgl.h"

void ui_tab_report_create(lv_obj_t *parent);

/**
 * 设置 AI 健康建议卡片正文(云端 LLM 生成的中文文本, UTF-8)。
 * 可从任意线程调用(如 MQTT 接收线程)——内部只拷 buffer + 置脏标志，
 * 真正刷 LVGL 在 report tab 的定时器(LVGL 线程)里做，跨线程安全。
 * @param utf8 UTF-8 文本; NULL 或空串则恢复占位提示。
 */
void ui_report_set_advice(const char *utf8);

#endif /* __UI_TAB_REPORT_H__ */
