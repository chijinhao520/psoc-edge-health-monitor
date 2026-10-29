#ifndef UI_CHECKUP_H
#define UI_CHECKUP_H
#include "checkup_flow.h"
/* LVGL线程专用；快照只保留本次引导体检，不是掉电保存的历史。 */
const checkup_flow_t *ui_checkup_get(void);
void ui_checkup_start(void);
void ui_checkup_retry(unsigned item);
void ui_checkup_skip(void);
void ui_checkup_cancel(void);
void ui_checkup_report(struct ui_health_data *out);
#endif
