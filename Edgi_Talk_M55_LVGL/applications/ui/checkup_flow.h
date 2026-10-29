#ifndef CHECKUP_FLOW_H
#define CHECKUP_FLOW_H
/* 纯状态机，毫秒时钟；同一LVGL线程调用。头内实现使Studio/SCons共享同一编译入口。 */
#include "ui_data.h"
#include <string.h>

enum { CHECKUP_IDLE, CHECKUP_PPG, CHECKUP_PPG_FAILED, CHECKUP_ECG,
       CHECKUP_ECG_FAILED, CHECKUP_DONE, CHECKUP_CANCELLED };
enum { CHECKUP_ACT_NONE, CHECKUP_ACT_PPG, CHECKUP_ACT_ECG };
enum { CHECKUP_FINGER=1, CHECKUP_HEART=2 };
typedef struct {
    unsigned id, state, resolved, completed;
    uint32_t started_ms, stage_ms, finished_ms, ppg_before, ecg_request;
    int reason;
    struct ui_health_data report;
} checkup_flow_t;

static inline int checkup_active(const checkup_flow_t *f)
{
    return f->state>=CHECKUP_PPG && f->state<=CHECKUP_ECG_FAILED;
}
static inline int checkup_select(checkup_flow_t *f, unsigned item, uint32_t now,
                                 const struct ui_health_data *d)
{
    f->resolved &= ~item; f->completed &= ~item;
    f->reason=0; f->stage_ms=now;
    if (item==CHECKUP_FINGER) {
        f->report.hr_valid=f->report.spo2_valid=f->report.glucose_valid=0;
        f->ppg_before=d->ppg_measurement_id;
        f->state=CHECKUP_PPG;
        return CHECKUP_ACT_PPG;
    }
    f->report.arr_valid=f->report.arr_pvc_valid=0;
    f->report.hrv_valid=f->report.resp_valid=0;
    f->ecg_request=0; f->state=CHECKUP_ECG;
    return CHECKUP_ACT_ECG;
}
static inline int checkup_start(checkup_flow_t *f,uint32_t now,const struct ui_health_data *d)
{
    unsigned id=f->id+1;
    memset(f,0,sizeof(*f)); f->id=id?id:1; f->started_ms=now;
    return checkup_select(f,CHECKUP_FINGER,now,d);
}
static inline int checkup_advance(checkup_flow_t *f,uint32_t now,const struct ui_health_data *d)
{
    if (!(f->resolved&CHECKUP_HEART)) return checkup_select(f,CHECKUP_HEART,now,d);
    f->state=CHECKUP_DONE; f->finished_ms=now;
    return CHECKUP_ACT_NONE;
}
static inline int checkup_skip(checkup_flow_t *f,uint32_t now,const struct ui_health_data *d)
{
    if (!checkup_active(f)) return CHECKUP_ACT_NONE;
    unsigned item=(f->state==CHECKUP_PPG || f->state==CHECKUP_PPG_FAILED)?CHECKUP_FINGER:CHECKUP_HEART;
    f->resolved|=item;
    return checkup_advance(f,now,d);
}
static inline void checkup_cancel(checkup_flow_t *f,uint32_t now)
{
    if (checkup_active(f)) { f->state=CHECKUP_CANCELLED; f->finished_ms=now; }
}
static inline int checkup_update(checkup_flow_t *f,uint32_t now,const struct ui_health_data *d)
{
    if (f->state==CHECKUP_PPG) {
        if (d->sensor_link_valid && d->ppg_session_state==UI_MEAS_DONE &&
            d->hr_valid && d->spo2_valid && d->ppg_measurement_id!=f->ppg_before) {
            f->report.heart_rate=d->heart_rate; f->report.spo2=d->spo2;
            f->report.hr_valid=f->report.spo2_valid=1;
            f->report.ppg_measurement_id=d->ppg_measurement_id;
            f->completed|=CHECKUP_FINGER; f->resolved|=CHECKUP_FINGER;
            return checkup_advance(f,now,d);
        }
        if (d->ppg_session_state==UI_MEAS_FAILED || d->ppg_session_state==UI_MEAS_CANCELLED ||
            d->ppg_session_state==UI_MEAS_EXPIRED || now-f->stage_ms>=100000u) {
            f->state=CHECKUP_PPG_FAILED;
            f->reason=d->ppg_reject_reason?d->ppg_reject_reason:UI_REJ_TIMEOUT;
        }
    } else if (f->state==CHECKUP_ECG) {
        if (f->ecg_request && d->ecg_session_id==f->ecg_request && d->sensor_link_valid &&
            d->arr_valid && d->arr_pvc_valid && d->hrv_valid) {
            f->report.arr_class=d->arr_class; f->report.arr_source=d->arr_source;
            f->report.arr_beat_count=d->arr_beat_count; f->report.arr_pvc_per_min=d->arr_pvc_per_min;
            for (unsigned i=0;i<4;i++) f->report.arr_class_count[i]=d->arr_class_count[i];
            f->report.arr_valid=f->report.arr_pvc_valid=1;
            f->report.hrv_valid=1; f->report.hrv_sdnn_ms=d->hrv_sdnn_ms;
            f->report.hrv_rmssd_ms=d->hrv_rmssd_ms;
            f->report.resp_valid=d->resp_valid; f->report.resp_rate_bpm=d->resp_rate_bpm;
            f->completed|=CHECKUP_HEART; f->resolved|=CHECKUP_HEART;
            return checkup_advance(f,now,d);
        }
        if (now-f->stage_ms>=100000u) { f->state=CHECKUP_ECG_FAILED; f->reason=UI_REJ_TIMEOUT; }
    }
    return CHECKUP_ACT_NONE;
}
#endif
