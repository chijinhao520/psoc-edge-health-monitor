/*
 * health_report.h — 体检报告"解读层"(把原始指标翻译成人看得懂的结论)
 *
 * 定位：这是"数字仪表盘 → 体检报告"的那一层。UI 只负责画卡片，所有
 * "这个数正不正常 / 该给什么建议 / 综合评级是几档"的判断都收在这里，
 * 跟 health_score.c(打分) / health_metrics.c(算 HRV/呼吸) 同层，纯 C、
 * 不依赖 LVGL，可单元测试。
 *
 * 诚实口径(沿用项目一贯做法)：
 *   - 呼吸率(RSA 简化法) / 血糖(67 样本物理上限) 是**实验性**指标，
 *     异常也只给"复测/参考"级建议，绝不出"就医"结论。
 *   - 只有心律(早搏) / 血氧 这类有临床意义且传感器可信的项，才会给
 *     "建议就医"级建议。
 *   - 免责声明常驻页脚：本报告仅供健康参考，不作医疗诊断。
 */

#ifndef __HEALTH_REPORT_H__
#define __HEALTH_REPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 汇总行片段(与 %d 计数拼接)：UI 与本模块共享，避免各写一份 hex 转义 */
#define HRPT_SUM_NORMAL "\xe9\xa1\xb9\xe6\xad\xa3\xe5\xb8\xb8"          /* 项正常 */
#define HRPT_SUM_WATCH  "\xe9\xa1\xb9\xe9\x9c\x80\xe5\x85\xb3\xe6\xb3\xa8"  /* 项需关注 */
#define HRPT_DISCLAIMER "\xe6\x9c\xac\xe6\x8a\xa5\xe5\x91\x8a\xe5\x9f\xba\xe4\xba\x8e\xe4\xbc\xa0\xe6\x84\x9f\xe5\x99\xa8\xe4\xbc\xb0\xe7\xae\x97\xef\xbc\x8c\xe4\xbb\x85\xe4\xbe\x9b\xe5\x81\xa5\xe5\xba\xb7\xe5\x8f\x82\xe8\x80\x83\xef\xbc\x8c\xe4\xb8\x8d\xe4\xbd\x9c\xe4\xb8\xba\xe5\x8c\xbb\xe7\x96\x97\xe8\xaf\x8a\xe6\x96\xad\xe4\xbe\x9d\xe6\x8d\xae\xe3\x80\x82"  /* 本报告基于传感器估算，仅供健康参考，不作为医疗诊断依据。 */

/* 单项/评级的严重度等级 → UI 用它映射颜色(灰/绿/橙/红) */
#define HRPT_LV_NA      0   /* 未测/无效 → 灰 */
#define HRPT_LV_OK      1   /* 正常      → 绿 */
#define HRPT_LV_WARN    2   /* 偏离/关注 → 橙 */
#define HRPT_LV_DANGER  3   /* 异常      → 红 */

/* 报告覆盖的指标(数组下标，UI 按此顺序取用) */
enum {
    HRPT_M_HR = 0,   /* 心率   */
    HRPT_M_SPO2,     /* 血氧   */
    HRPT_M_RHYTHM,   /* 心律   */
    HRPT_M_RESP,     /* 呼吸(实验性) */
    HRPT_M_HRV,      /* 心率变异 SDNN */
    HRPT_M_STRESS,   /* 应激(GSR) */
    HRPT_M_GLU,      /* 血糖(实验性) */
    HRPT_M_N
};

#define HRPT_MAX_ADVICE  4

/* 单项评估结果 */
typedef struct {
    int         level;        /* HRPT_LV_* */
    const char *status_text;  /* 正常/偏高/偏低/... (静态 UTF-8，永不为 NULL) */
} hrpt_eval_t;

/* 一次完整的报告解读 */
typedef struct {
    int         score;          /* 0-100 综合评分(直接沿用 health_score 结果) */
    int         score_valid;
    int         grade_level;    /* HRPT_LV_* (给评级词上色) */
    const char *grade_text;     /* 优秀/良好/一般/需关注/评估中 */

    int         normal_count;   /* 正常项数(level==OK) */
    int         watch_count;    /* 需留意项数(level==WARN||DANGER) */

    hrpt_eval_t m[HRPT_M_N];    /* 逐项状态，下标见上面 enum */

    const char *advice[HRPT_MAX_ADVICE]; /* 按严重度排序的建议句(静态 UTF-8) */
    int         advice_count;

    const char *disclaimer;     /* 免责声明页脚(常驻) */
} health_report_t;

/* health_report_build 的输入：跟 ui_health_data 的字段一一对应，
 * 由 UI 层从快照填好后传入，本模块不碰 IPC/任务结构，纯函数。 */
typedef struct {
    int      heart_rate;      int hr_valid;
    float    spo2;            int spo2_valid;
    int      arr_valid;       int arr_class;
    unsigned arr_beat_count;  unsigned arr_pvc_per_min;
    float    hrv_sdnn_ms;     int hrv_valid;
    float    resp_rate_bpm;   int resp_valid;
    float    gsr_stress;      int gsr_valid;
    float    glucose_mmol;    int glucose_valid;
    int      health_score;    int health_score_valid;
} health_report_input_t;

/**
 * @brief 用当前各项指标生成一次体检报告解读。
 * @param in   输入指标(见 health_report_input_t，UI 从数据快照填)
 * @param out  输出解读结果(调用方分配)
 */
void health_report_build(const health_report_input_t *in, health_report_t *out);

/** @brief 单项参考范围文字(静态 ASCII，如 "60-100")，供 UI 建卡时贴静态副标题。
 *  @param metric HRPT_M_* 下标 */
const char *health_report_ref_text(int metric);

#ifdef __cplusplus
}
#endif

#endif /* __HEALTH_REPORT_H__ */
