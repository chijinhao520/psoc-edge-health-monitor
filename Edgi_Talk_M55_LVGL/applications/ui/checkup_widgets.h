#ifndef CHECKUP_WIDGETS_H
#define CHECKUP_WIDGETS_H
#include "ui_checkup.h"
#include "ui_common.h"
#include "ui_cn_font.h"

typedef struct {
    lv_obj_t *card, *title, *hint, *items, *note;
    lv_obj_t *buttons[4], *labels[4];
    int chinese;
} checkup_widgets_t;
void ui_show_report(void);

static inline const char *checkup_reason(int reason)
{
    switch (reason) {
    case UI_REJ_NO_FINGER: return "请让指腹覆盖传感器，保持接触。";
    case UI_REJ_MOTION: return "波形不稳，请放松手指、保持不动。";
    case UI_REJ_SENSOR: return "传感器未连接，请检查连接后重试。";
    case UI_REJ_LINK: return "采集端未连接，请检查设备连接。";
    case UI_REJ_REQUEST: return "设备未确认请求，可重试或跳过。";
    case UI_REJ_SPO2: case UI_REJ_HR: return "结果超出当前支持范围，请重新测量；不能据此判断正常。";
    case UI_REJ_TIMEOUT: return "等待已超时，可调整后重试或跳过。";
    default: return "暂未获得可靠信号，请检查接触并保持不动。";
    }
}
static inline void checkup_button_cb(lv_event_t *e)
{
    unsigned button=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    const checkup_flow_t *f=ui_checkup_get();
    if (button==3) { ui_show_report(); return; }
    if (f->state==CHECKUP_DONE) {
        if (!button) ui_checkup_start();
        else ui_checkup_retry(button==1?CHECKUP_FINGER:CHECKUP_HEART);
    } else if (checkup_active(f)) {
        if (!button && (f->state==CHECKUP_PPG_FAILED || f->state==CHECKUP_ECG_FAILED))
            ui_checkup_retry(f->state==CHECKUP_PPG_FAILED?CHECKUP_FINGER:CHECKUP_HEART);
        else if (button==1) ui_checkup_skip();
        else if (button==2) ui_checkup_cancel();
    } else if (!button) ui_checkup_start();
}
static inline void checkup_widgets_refresh(checkup_widgets_t *w)
{
    const checkup_flow_t *f=ui_checkup_get();
    const struct ui_health_data *d=ui_data_get();
    char text[320];
    if (w->chinese) {
        if (f->id) lv_snprintf(text,sizeof(text),"本次体检 #%u · %s",f->id,
            f->state==CHECKUP_DONE?(f->completed==3?"检测完成":"部分结果"):
            f->state==CHECKUP_CANCELLED?"已取消":
            (f->state==CHECKUP_PPG_FAILED || f->state==CHECKUP_ECG_FAILED)?"等待重试":"进行中");
        else lv_snprintf(text,sizeof(text),"引导体检 · 手指检测 / 心电检测");
    } else lv_snprintf(text,sizeof(text),"Checkup #%u | state %u",f->id,f->state);
    lv_label_set_text(w->title,text);
    const char *hint="点击开始体检，按步骤完成检测；缺少设备可跳过。";
    if (f->state==CHECKUP_PPG) {
        hint="步骤 1/2：指腹覆盖传感器，保持自然呼吸。";
        if (d->ppg_session_state==UI_MEAS_SETTLING) hint="正在稳定信号，请保持手指不动，避免用力按压。";
        if (d->ppg_session_state==UI_MEAS_MEASURING)
            hint=d->ppg_reject_reason?checkup_reason(d->ppg_reject_reason):"正在采样，请保持不动。";
    } else if (f->state==CHECKUP_PPG_FAILED) hint=checkup_reason(f->reason);
    else if (f->state==CHECKUP_ECG) {
        hint=(!d->sensor_link_valid)?"采集端未连接，请检查设备连接。":
             d->ecg_session_id!=f->ecg_request?"步骤 2/2：正在准备本次心电检测。":
             !d->arr_valid?"步骤 2/2：请按设备说明连接心电电极，保持静止；尚无有效心拍。":
             "已检测到心拍，正在积累心律和 HRV 数据，通常需持续一分钟。";
    } else if (f->state==CHECKUP_ECG_FAILED) hint="未在规定时间内获得完整心电结果。请检查电极，可单独重试或跳过。";
    else if (f->state==CHECKUP_DONE) hint="本次结果已保留，可查看报告；重测单项会替换该项。";
    else if (f->state==CHECKUP_CANCELLED) hint="体检已取消，仅保留此前完成项目；开始新体检会清除本次结果。";
    lv_label_set_text(w->hint,w->chinese?hint:"Follow finger sensor, then ECG electrodes. Retry or skip unavailable steps.");
    unsigned elapsed=(checkup_active(f)?lv_tick_get():f->finished_ms)-f->started_ms;
    if (w->chinese) lv_snprintf(text,sizeof(text),"心率 / 血氧：%s\n心律 / HRV：%s\n本次用时：%u 秒%s",
        f->completed&1?"已完成":f->resolved&1?"已跳过":"未完成",
        f->completed&2?"已完成":f->resolved&2?"已跳过":"未完成",f->id?elapsed/1000:0,
        f->completed&2?(f->report.arr_source?" · 心律：规则分析":" · 心律：模型分析"):"");
    else lv_snprintf(text,sizeof(text),"Finger: %s | ECG: %s | %us",
        f->completed&1?"done":f->resolved&1?"skipped":"pending",
        f->completed&2?"done":f->resolved&2?"skipped":"pending",f->id?elapsed/1000:0);
    lv_label_set_text(w->items,text);
    lv_label_set_text(w->note,w->chinese?
        "本次快照重启后清除，缺项不代表正常。呼吸率为估计；环境、红外温度及 GSR 在监测页实时查看。":
        "Session snapshot; missing is not normal. Respiratory rate is an estimate.");
    if (!w->buttons[0]) return;
    const char *labels[4]={"开始体检","跳过本项","取消体检","查看报告"};
    const char *english[4]={"Start","Skip","Cancel","Report"};
    int active=checkup_active(f);
    int failed=f->state==CHECKUP_PPG_FAILED || f->state==CHECKUP_ECG_FAILED;
    if (f->state==CHECKUP_DONE) { labels[0]="新体检";labels[1]="重测手指";labels[2]="重测心电";
        english[0]="New";english[1]="Finger";english[2]="ECG"; }
    else if (active) { labels[0]=failed?"重试本项":"检测中";english[0]=failed?"Retry":"Running"; }
    for (unsigned i=0;i<4;i++) {
        lv_label_set_text(w->labels[i],w->chinese?labels[i]:english[i]);
        int enabled=i==0?(!active || failed):i==3?f->id!=0:(active || f->state==CHECKUP_DONE);
        if (enabled) lv_obj_clear_state(w->buttons[i],LV_STATE_DISABLED);
        else lv_obj_add_state(w->buttons[i],LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(w->buttons[i],enabled?UI_COLOR_SPO2:UI_COLOR_DIVIDER,0);
        lv_obj_set_style_text_color(w->labels[i],enabled?lv_color_white():UI_COLOR_TEXT_SEC,0);
    }
}
static inline void checkup_widgets_create(checkup_widgets_t *w,lv_obj_t *parent,int controls)
{
    lv_font_t *font=ui_cn_font_get(18);
    w->chinese=font!=NULL;
    if (!font) font=(lv_font_t *)&ui_font_Subtitle;
    w->card=ui_card_create(parent,UI_CARD_FULL_W,LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(w->card,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(w->card,8,0);
    lv_obj_t **labels[]={&w->title,&w->hint,&w->items,&w->note};
    for (unsigned i=0;i<4;i++) {
        *labels[i]=lv_label_create(w->card);
        lv_obj_set_width(*labels[i],UI_CARD_FULL_W-32);
        lv_label_set_long_mode(*labels[i],LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(*labels[i],font,0);
        lv_obj_set_style_text_color(*labels[i],i==3?UI_COLOR_TEXT_SEC:UI_COLOR_TEXT_PRI,0);
    }
    if (controls) {
        lv_obj_t *row=lv_obj_create(w->card); lv_obj_remove_style_all(row);
        lv_obj_set_size(row,UI_CARD_FULL_W-32,48);
        lv_obj_set_flex_flow(row,LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row,4,0);
        lv_obj_clear_flag(row,LV_OBJ_FLAG_SCROLLABLE);
        for (unsigned i=0;i<4;i++) {
            w->buttons[i]=lv_button_create(row);
            lv_obj_set_width(w->buttons[i],(UI_CARD_FULL_W-44)/4);
            lv_obj_set_height(w->buttons[i],44);
            lv_obj_set_style_pad_all(w->buttons[i],2,0);
            lv_obj_add_event_cb(w->buttons[i],checkup_button_cb,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
            w->labels[i]=lv_label_create(w->buttons[i]);
            lv_obj_set_style_text_font(w->labels[i],font,0);
            lv_obj_center(w->labels[i]);
        }
    }
    checkup_widgets_refresh(w);
}
#endif
