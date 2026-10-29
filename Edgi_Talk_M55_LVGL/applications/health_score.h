/*
 * health_score.h — 规则式综合健康评分(0-100)
 *
 * 不训练模型，纯加权扣分制：每项指标只在自身 valid 时才参与扣分，
 * 缺项(比如刚开机 HRV 还没攒够样本)不影响其余项，不因为"还没数据"
 * 就把分打低。权重是经验取值，用于体检报告展示，不是医学诊断结论。
 */

#ifndef __HEALTH_SCORE_H__
#define __HEALTH_SCORE_H__

#include "arrhythmia_task.h"
#include "health_metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int score;   /* 0-100 */
    int valid;   /* 至少 HR 或 SpO2 有效才认为评分有意义 */
} health_score_result_t;

/**
 * @brief 用当前各项已算好的指标算一次健康评分。
 *
 * @param heart_rate   心率(bpm)
 * @param hr_valid     心率是否有效
 * @param spo2         血氧饱和度(%)
 * @param spo2_valid   血氧是否有效
 * @param arr          心律不齐结果(可传 NULL 跳过这部分扣分)
 * @param hm           HRV/呼吸率结果(可传 NULL 跳过这部分扣分)
 * @param glucose_mmol 血糖参考值(mmol/L)
 * @param glucose_valid 血糖是否有效
 */
health_score_result_t health_score_compute(
    int heart_rate, int hr_valid,
    float spo2, int spo2_valid,
    const arr_result_t *arr,
    const health_metrics_result_t *hm,
    float glucose_mmol, int glucose_valid);

#ifdef __cplusplus
}
#endif

#endif /* __HEALTH_SCORE_H__ */
