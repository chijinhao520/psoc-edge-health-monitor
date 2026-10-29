/*
 * health_score.c — 规则式综合健康评分实现
 */

#include "health_score.h"
#include <stddef.h>

/* 血糖参考范围，跟 ui_data.h 的 GLUCOSE_LOW/HIGH 保持一致 */
#define HS_GLUCOSE_LOW   3.9f
#define HS_GLUCOSE_HIGH  6.1f

health_score_result_t health_score_compute(
    int heart_rate, int hr_valid,
    float spo2, int spo2_valid,
    const arr_result_t *arr,
    const health_metrics_result_t *hm,
    float glucose_mmol, int glucose_valid)
{
    health_score_result_t r;
    int score = 100;
    int have_vital = 0;

    if (hr_valid)
    {
        have_vital = 1;
        if (heart_rate < 50 || heart_rate > 110) score -= 15;
        if (heart_rate < 40 || heart_rate > 140) score -= 15;  /* 极端值叠加再扣 */
    }

    if (spo2_valid)
    {
        have_vital = 1;
        if (spo2 < 95.0f) score -= 10;
        if (spo2 < 90.0f) score -= 20;
    }

    if (arr != NULL && arr->valid && arr->beat_count > 0)
    {
        /* PVC 负荷：>6次/分钟属临床关注范围(粗判，非诊断) */
        if (arr->pvc_valid && arr->pvc_per_min > 6)       score -= 15;
        else if (arr->pvc_valid && arr->pvc_per_min > 0)  score -= 5;

        /* 非正常(S/V/F)心拍占比过高也扣分 */
        if (arr->beat_count > 10)
        {
            uint32_t abnormal = arr->cls_count[1] + arr->cls_count[2] + arr->cls_count[3];
            float ratio = (float)abnormal / (float)arr->beat_count;
            if (ratio > 0.30f)      score -= 15;
            else if (ratio > 0.10f) score -= 5;
        }
    }

    if (hm != NULL && hm->hrv_valid && hm->hrv_sdnn_ms < 20.0f)
        score -= 5;   /* HRV 偏低，权重小，仅作为次要参考 */

    if (hm != NULL && hm->resp_valid &&
        (hm->resp_rate_bpm < 10.0f || hm->resp_rate_bpm > 24.0f))
        score -= 10;

    if (glucose_valid && (glucose_mmol < HS_GLUCOSE_LOW || glucose_mmol > HS_GLUCOSE_HIGH))
        score -= 5;   /* 血糖是参考性指标(实验性)，权重小 */

    if (score < 0)   score = 0;
    if (score > 100) score = 100;

    r.score = score;
    r.valid = have_vital;
    return r;
}
