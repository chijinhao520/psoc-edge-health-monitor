/**
 * @file  ui_data.c
 * @brief Health data provider for LVGL UI.
 *
 * 切换宏：
 *   #define UI_DATA_USE_IPC 0  → 假数据定时器（P1.5 原型）
 *   #define UI_DATA_USE_IPC 1  → IPC 共享内存读取（4月双核集成）
 *
 * 切换后重编译 M55 工程即可，其余 UI 代码零改动。
 */

/* ------------------------------------------------------------------ */
/*  切换宏：0 = 假数据，1 = IPC 真实数据                               */
/* ------------------------------------------------------------------ */
#define UI_DATA_USE_IPC 1

#include "ui_data.h"
#include "ui_checkup.h"
#include <stdlib.h>
#include <rtthread.h>

static checkup_flow_t s_checkup;
static void checkup_poll_cb(lv_timer_t *t);

#if UI_DATA_USE_IPC
#include "ipc_shared_data.h"
#include "ipc_snapshot.h"
#include "../glucose_task.h"
#include "../arrhythmia_task.h"
#include "../health_metrics.h"
#include "../health_score.h"
#ifdef RT_USING_AUDIO
#include "../voice_report.h"
#endif
#include <board.h>   /* 引入 BSP/CMSIS 头文件，提供 SCB_InvalidateDCache_by_Addr */
#endif

/* ------------------------------------------------------------------ */
/*  内部状态                                                            */
/* ------------------------------------------------------------------ */

/*
 * 初值一律为"未测"。
 *
 * 曾经这里写的是 glucose=5.2 / heart_rate=75 / spo2=98 这类"像样"的假值
 * （UI_DATA_USE_IPC=0 假数据模式的遗留），配合监测页不查 valid 直接显示，
 * 结果是：一次都没测过，屏幕上照样有"血糖 5.2 正常""心率 75"。
 * 这对体检设备是硬伤 —— 宁可显示 "--"，也不能编数。
 *
 * 数值字段留 0 仅为占位，UI 必须靠 *_valid 判断能不能显示。
 */
static struct ui_health_data s_data = {
    .glucose        = 0.0f,
    .glucose_valid  = 0,
    .heart_rate     = 0,
    .hr_valid       = 0,
    .spo2           = 0.0f,
    .spo2_valid     = 0,
    .body_temp      = 0.0f,
    .bmr            = 0.0f,
    .env_temp       = 0.0f,
    .env_humidity   = 0.0f,
    .blood_flow     = 0.0f,
};

/* ------------------------------------------------------------------ */
/*  假数据定时器（UI_DATA_USE_IPC = 0）                                */
/* ------------------------------------------------------------------ */

#if !UI_DATA_USE_IPC

static float vary(float base, float range)
{
    int r = rand() % 200 - 100;
    return base + range * r / 100.0f;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void fake_data_cb(lv_timer_t *t)
{
    (void)t;

    s_data.glucose      = clampf(vary(5.2f,  0.4f),  3.5f, 8.0f);
    s_data.heart_rate   = (int)clampf(vary(75.0f, 8.0f), 50.0f, 120.0f);
    s_data.spo2         = clampf(vary(98.0f, 0.8f),  90.0f, 100.0f);
    s_data.body_temp    = clampf(vary(36.5f, 0.3f),  35.5f, 37.8f);
    s_data.bmr          = clampf(vary(42.8f, 2.0f),  35.0f, 55.0f);
    s_data.env_temp     = clampf(vary(25.3f, 0.5f),  20.0f, 32.0f);
    s_data.env_humidity = clampf(vary(55.2f, 2.5f),  30.0f, 80.0f);
    s_data.blood_flow   = clampf(vary(1.2f,  0.15f), 0.5f,  2.0f);
}

#endif /* !UI_DATA_USE_IPC */

/* ------------------------------------------------------------------ */
/*  IPC 轮询定时器（UI_DATA_USE_IPC = 1）                              */
/* ------------------------------------------------------------------ */

#if UI_DATA_USE_IPC

static uint32_t s_last_seq;
static rt_tick_t s_last_change;
static uint32_t s_last_measurement;
static uint32_t s_pending_cmd;
static rt_tick_t s_pending_since;

static void ipc_poll_cb(lv_timer_t *t)
{
    (void)t;
    sensor_ipc_data_t snap;
    struct ui_health_data next;
    ui_data_copy(&next);
    rt_tick_t now=rt_tick_get();
    int stable=ipc_sensor_snapshot(&snap);
    if (stable && snap.sequence!=s_last_seq) {
        s_last_seq=snap.sequence; s_last_change=now;
    }
    int fresh=stable && now-s_last_change<=rt_tick_from_millisecond(3000);
    next.sensor_link_valid=fresh;
    /* A pending start must first be observed by M33; do not restore the old result. */
    if (fresh && s_pending_cmd && snap.ppg_request_seq==s_pending_cmd) s_pending_cmd=0;
    int ppg=fresh && snap.ppg_valid && !s_pending_cmd;
    next.hr_valid=next.spo2_valid=ppg;
    next.ppg_measurement_id=fresh ? snap.ppg_measurement_id : 0;
    next.heart_rate=ppg ? snap.heart_rate : 0;
    next.spo2=ppg ? (float)snap.spo2 : 0.0f;
    next.blood_flow=0;
    next.body_temp_valid=fresh && snap.mlx_valid;
    next.body_temp=next.body_temp_valid ? snap.t_ir : 0;
    next.env_valid=fresh && snap.sht30_valid;
    next.env_temp=next.env_valid ? snap.sht30_temp : 0;
    next.env_humidity=next.env_valid ? snap.sht30_humi : 0;
    next.bmr_valid=0; next.bmr=0;
    next.gsr_valid=fresh && snap.gsr_valid;
    next.gsr_stress_index=next.gsr_valid ? snap.gsr_stress_index : 0;
    next.pat_valid=0; next.pat_ms=0;
    if (fresh && !s_pending_cmd) {
        next.ppg_session_state=snap.ppg_session_state;
        next.ppg_session_progress=snap.ppg_session_progress;
        next.ppg_reject_reason=snap.ppg_reject_reason;
    }
    /* 保留未确认序号以阻断旧结果；重试/取消会发送新序号，不会恢复过期会话。 */
    if (s_pending_cmd && now-s_pending_since>=rt_tick_from_millisecond(10000)) {
        next.ppg_session_state=UI_MEAS_FAILED;
        next.ppg_reject_reason=UI_REJ_REQUEST;
        next.ppg_session_progress=0;
    } else if (!fresh && !s_pending_cmd) {
        next.ppg_session_state=UI_MEAS_FAILED;
        next.ppg_reject_reason=UI_REJ_LINK;
        next.ppg_session_progress=0;
    }
    if (fresh && !s_pending_cmd && snap.ppg_session_state==UI_MEAS_DONE && !ppg)
        next.ppg_session_state=UI_MEAS_EXPIRED;
    /* Local results update every poll, independently of the M33 scalar sequence. */
    glucose_result_t gr;
    arr_result_t ar;
    health_metrics_result_t hm;
    glucose_copy_result(&gr);
    arrhythmia_copy_result(&ar);
    health_metrics_copy_result(&hm);
    next.glucose_valid=ppg && gr.valid && gr.measurement_id==snap.ppg_measurement_id;
    next.glucose=next.glucose_valid ? gr.glucose : 0;
    next.arr_valid=ar.valid;
    next.ecg_session_id=ar.session_id;
    next.arr_class=ar.last_class; next.arr_source=ar.source;
    next.arr_beat_count=ar.beat_count; next.arr_pvc_per_min=ar.pvc_per_min;
    for (unsigned i=0;i<4;i++) next.arr_class_count[i]=ar.cls_count[i];
    next.arr_pvc_valid=ar.valid && ar.pvc_valid;
    next.hrv_valid=hm.hrv_valid; next.hrv_rmssd_ms=hm.hrv_rmssd_ms;
    next.hrv_sdnn_ms=hm.hrv_sdnn_ms;
    next.resp_valid=hm.resp_valid; next.resp_rate_bpm=hm.resp_rate_bpm;
    health_score_result_t hs=health_score_compute(next.heart_rate,next.hr_valid,
        next.spo2,next.spo2_valid,&ar,&hm,next.glucose,next.glucose_valid);
    next.health_score=hs.score; next.health_score_valid=hs.valid;
    rt_base_t level=rt_hw_interrupt_disable();
    s_data=next;
    rt_hw_interrupt_enable(level);
#ifdef RT_USING_AUDIO
    if (ppg && snap.ppg_session_state==UI_MEAS_DONE &&
        snap.ppg_measurement_id!=s_last_measurement) voice_report_request();
#endif
    if (ppg) s_last_measurement=snap.ppg_measurement_id;
}

#endif /* UI_DATA_USE_IPC */

/* ------------------------------------------------------------------ */
/*  公共 API                                                            */
/* ------------------------------------------------------------------ */

void ui_data_init(void)
{
    lv_timer_create(checkup_poll_cb, 500, NULL);
#if UI_DATA_USE_IPC
    lv_timer_create(ipc_poll_cb, 500, NULL);   /* 500ms 轮询，减少感知延迟 */
#else
    lv_timer_create(fake_data_cb, 2000, NULL);  /* 2s 假数据 */
#endif
}

const struct ui_health_data *ui_data_get(void)
{
    return &s_data;
}

void ui_data_copy(struct ui_health_data *out)
{
    if (!out) return;
    rt_base_t level=rt_hw_interrupt_disable();
    *out=s_data;
    rt_hw_interrupt_enable(level);
}

static void send_measure_command(int command)
{
#if UI_DATA_USE_IPC
    /*
     * 反向 IPC：M55 → M33。写命令后必须 SCB_CleanDCache_by_Addr 把 M55 的
     * D-Cache 刷回物理 SRAM，否则 M33（无 cache，直读内存）看到的还是旧值。
     * 与全项目其余"M33 写 M55 invalidate 读"方向相反，是唯一反方向通道。
     *
     * 先写 cmd/magic 再递增 seq，且 seq 最后 clean —— 保证 M33 看到新 seq 时
     * cmd 一定已就绪（M33 靠 seq 变化触发，不能先看到 seq 后看到旧 cmd）。
     */
    volatile ppg_cmd_ipc_t *c = ipc_get_cmd_ptr();
    uint32_t next = (c->magic == IPC_CMD_MAGIC) ? (c->seq + 1) : 1;
    if (!next) next=1; /* 0 表示无待处理请求 */

    s_pending_cmd=next;
    s_pending_since=rt_tick_get();
    rt_base_t level=rt_hw_interrupt_disable();
    s_data.hr_valid=s_data.spo2_valid=s_data.glucose_valid=0;
    s_data.bmr_valid=s_data.pat_valid=0;
    s_data.ppg_session_state=command==PPG_CMD_CANCEL_MEASURE ? UI_MEAS_CANCELLING : UI_MEAS_WAIT_FINGER;
    s_data.ppg_reject_reason=UI_REJ_PASS;
    s_data.ppg_session_progress=0;
    rt_hw_interrupt_enable(level);
    c->cmd   = command;
    c->magic = IPC_CMD_MAGIC;
    c->seq   = next;

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanDCache_by_Addr((uint32_t *)IPC_CMD_ADDR, sizeof(ppg_cmd_ipc_t));
#endif
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

void ui_data_send_start_measure(void) {
#if UI_DATA_USE_IPC
    send_measure_command(PPG_CMD_START_MEASURE);
#endif
}
void ui_data_send_cancel_measure(void) {
#if UI_DATA_USE_IPC
    send_measure_command(PPG_CMD_CANCEL_MEASURE);
#endif
}

static void checkup_action(int action)
{
    if (action==CHECKUP_ACT_PPG) ui_data_send_start_measure();
#if UI_DATA_USE_IPC
    if (action==CHECKUP_ACT_ECG) s_checkup.ecg_request=arrhythmia_request_session();
#endif
}
static void checkup_poll_cb(lv_timer_t *t)
{
    (void)t;
    checkup_action(checkup_update(&s_checkup,lv_tick_get(),ui_data_get()));
}
const checkup_flow_t *ui_checkup_get(void) { return &s_checkup; }
void ui_checkup_start(void)
{
    if (!checkup_active(&s_checkup))
        checkup_action(checkup_start(&s_checkup,lv_tick_get(),ui_data_get()));
}
void ui_checkup_retry(unsigned item)
{
    if (!s_checkup.id || (item!=CHECKUP_FINGER && item!=CHECKUP_HEART)) return;
    if (s_checkup.state!=CHECKUP_DONE && s_checkup.state!=CHECKUP_PPG_FAILED &&
        s_checkup.state!=CHECKUP_ECG_FAILED) return;
    checkup_action(checkup_select(&s_checkup,item,lv_tick_get(),ui_data_get()));
}
void ui_checkup_skip(void)
{
    if (s_checkup.state==CHECKUP_PPG || s_checkup.state==CHECKUP_PPG_FAILED)
        ui_data_send_cancel_measure();
    checkup_action(checkup_skip(&s_checkup,lv_tick_get(),ui_data_get()));
}
void ui_checkup_cancel(void)
{
    if (s_checkup.state==CHECKUP_PPG || s_checkup.state==CHECKUP_PPG_FAILED)
        ui_data_send_cancel_measure();
    checkup_cancel(&s_checkup,lv_tick_get());
}
void ui_checkup_report(struct ui_health_data *out)
{
    if (!out) return;
    if (!s_checkup.id) { ui_data_copy(out); return; }
    *out=s_checkup.report;
    /* 仅使用本次已收集的项目计算规则评分，缺项时不提供总评。 */
#if UI_DATA_USE_IPC
    if (s_checkup.state==CHECKUP_DONE && s_checkup.completed==3) {
        arr_result_t ar={0}; health_metrics_result_t hm={0};
        ar.valid=out->arr_valid; ar.pvc_valid=out->arr_pvc_valid;
        ar.source=out->arr_source; ar.last_class=out->arr_class; ar.pvc_per_min=out->arr_pvc_per_min;
        ar.beat_count=out->arr_beat_count;
        for (unsigned i=0;i<4;i++) ar.cls_count[i]=out->arr_class_count[i];
        hm.hrv_valid=out->hrv_valid; hm.hrv_sdnn_ms=out->hrv_sdnn_ms; hm.hrv_rmssd_ms=out->hrv_rmssd_ms;
        hm.resp_valid=out->resp_valid; hm.resp_rate_bpm=out->resp_rate_bpm;
        health_score_result_t hs=health_score_compute(out->heart_rate,out->hr_valid,out->spo2,
                                                     out->spo2_valid,&ar,&hm,0,0);
        out->health_score=hs.score; out->health_score_valid=hs.valid;
    }
#endif
}

/* 血糖历史环形缓冲已删（2026-07-17）：唯一消费者是旧血糖独立页的趋势
 * 折线图，随 5/28 战略转向（血糖降级为辅 AI）一并下线。要恢复趋势图时
 * 从 git 历史找 push_glucose / ui_data_get_glucose_history。 */
