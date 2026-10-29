#ifndef __UI_DATA_H__
#define __UI_DATA_H__

#include "lvgl.h"

/* ====== Health Data Structure ====== */
struct ui_health_data {
    int   sensor_link_valid; /* 采集核标量心跳在线，不代表全部传感器正常 */
    float glucose;         /* mmol/L  */
    int   heart_rate;      /* BPM     */
    float spo2;            /* %       */
    float body_temp;       /* deg C   */
    float bmr;             /* kcal/h; unavailable until a validated source exists */
    float env_temp;        /* deg C   */
    float env_humidity;    /* %RH     */
    float blood_flow;      /* a.u.    */

    /* --- 有效标志(体检报告"未测 vs 数值"判定用，2026-07-09 补) --- */
    int   hr_valid;        /* 心率有效(PPG ppg_valid) */
    int   spo2_valid;      /* 血氧有效(PPG ppg_valid) */
    int   glucose_valid;   /* 血糖有效(仅真实 NPU 推理；占位公式产物恒 0) */

    /* 有效标志随来源失效和超时撤回。 */
    int   body_temp_valid; /* 体温有效(IPC mlx_valid 透传) */
    int   bmr_valid;       /* BMR 有效；可信来源尚未建立，当前为 0 */
    int   env_valid;       /* 环境温湿度有效(外接 SHT30) */

    /* --- 体检报告(2026-07-08 新增) --- */
    int      arr_class;        /* 最近一拍心律分类 0=N 1=S 2=V 3=F */
    int      arr_valid;        /* 1 = 已有心律不齐分类结果 */
    int      arr_source;       /* 0=NPU 推理 1=规则兜底(与 arrhythmia_task.h
                                * 的 ARR_SRC_* 一一对应，跨模块契约)。
                                * 建议/报告文案提"NPU 推理得出"前必须查它 */
    unsigned arr_beat_count;   /* 累计分类心拍数 */
    uint32_t arr_class_count[4]; /* 与心律快照一起用于规则评分 */
    unsigned arr_pvc_per_min;  /* 近 1 分钟室早(V)数 */
    int      arr_pvc_valid;    /* 连续完整统计窗有效 */

    float hrv_rmssd_ms;    /* HRV RMSSD */
    float hrv_sdnn_ms;     /* HRV SDNN */
    int   hrv_valid;

    float resp_rate_bpm;   /* 呼吸率(实验性) */
    int   resp_valid;

    int   health_score;    /* 0-100 综合健康评分 */
    int   health_score_valid;

    /* --- GSR 应激 / PAT 脉搏波传导时间 (2026-07-09 补充 UI 展示) --- */
    float gsr_stress_index; /* 应激指数 0-100，相对基线的评分 */
    int   gsr_valid;
    float pat_ms;           /* 脉搏波传导时间 ms（ECG R峰 -> 主 PPG 峰）*/
    int   pat_valid;

    /* --- PPG 测量会话 (2026-07-16 新增) ---
     * 手指贴合由 M33 侧 DC 自动检测触发（无需按钮），UI 据此提示用户
     * "请贴手指 / 测量中 xx% / 测量完成"。取值见 ui_meas_state_t。 */
    int   ppg_session_state;    /* 取值见下方 ui_meas_state_t */
    int   ppg_session_progress; /* 0-100 */
    int   ppg_reject_reason;    /* 失败原因，取值见 ui_meas_reject_t */
    uint32_t ppg_measurement_id;
    uint32_t ecg_session_id;
};

/* 与 M33 algo/ppg_session.h 的 ppg_session_state_t 一一对应（跨核契约，
 * 改一侧必须同步改另一侧） */
typedef enum {
    UI_MEAS_IDLE        = 0,
    UI_MEAS_WAIT_FINGER = 1,
    UI_MEAS_SETTLING    = 2,
    UI_MEAS_MEASURING   = 3,
    UI_MEAS_DONE        = 4,
    UI_MEAS_FAILED      = 5,
    UI_MEAS_CANCELLED   = 6,
    UI_MEAS_CANCELLING  = 7, /* M55本地：等待M33受理取消 */
    UI_MEAS_EXPIRED     = 8, /* M55本地：完成结果已过期 */
} ui_meas_state_t;

/* 与 M33 algo/ppg_sqi.h 的 ppg_gate_result_t 一一对应（跨核契约，
 * 改一侧必须同步改另一侧）。UI 只关心能否转成一句可执行的人话 ——
 * 能给出明确指令的只有 NO_FINGER(手指没放好) 和 MOTION(手指在动)，
 * 其余几种对用户是同一件事："信号不稳，重测"。 */
typedef enum {
    UI_REJ_PASS      = 0,
    UI_REJ_NO_FINGER = 1,
    UI_REJ_ALGO      = 2,
    UI_REJ_SPO2      = 3,
    UI_REJ_HR        = 4,
    UI_REJ_SQI       = 5,
    UI_REJ_MOTION    = 6,
    UI_REJ_SENSOR    = 7,
    UI_REJ_TIMEOUT   = 8,
    UI_REJ_LINK      = 9, /* M55本地请求确认/心跳超时 */
    UI_REJ_REQUEST   = 10, /* 采集核未确认当前请求 */
} ui_meas_reject_t;

/** 发"开始测量"命令给 M33（反向 IPC，按钮回调调用） */
void ui_data_send_start_measure(void);
void ui_data_send_cancel_measure(void);

/* Glucose thresholds (mmol/L) */
#define GLUCOSE_LOW      3.9f
#define GLUCOSE_HIGH     6.1f
#define GLUCOSE_DANGER   7.0f

/* ====== API ====== */

/** Start the health-data provider (IPC poll timer; call once from lv_user_gui_init). */
void ui_data_init(void);
void ui_data_copy(struct ui_health_data *out);

/** Get latest snapshot (pointer valid until next timer tick). */
const struct ui_health_data *ui_data_get(void);

#endif /* __UI_DATA_H__ */
