/*
 * health_report.c — 体检报告解读层实现
 *
 * 中文串一律 UTF-8 hex 转义(项目约定，防编译器字符集问题)；
 * 这批宏由 scratchpad/gen_report_strings.py 生成，改文案请改脚本重生成。
 */

#include "health_report.h"
#include <stddef.h>

/* ==== 中文文案(gen_report_strings.py 生成的 UTF-8 hex 转义) ==== */
#define HRPT_GRADE_EXCELLENT "\xe4\xbc\x98\xe7\xa7\x80"  /* 优秀 */
#define HRPT_GRADE_GOOD      "\xe8\x89\xaf\xe5\xa5\xbd"  /* 良好 */
#define HRPT_GRADE_FAIR      "\xe4\xb8\x80\xe8\x88\xac"  /* 一般 */
#define HRPT_GRADE_ATTENTION "\xe9\x9c\x80\xe5\x85\xb3\xe6\xb3\xa8"  /* 需关注 */
#define HRPT_GRADE_PENDING   "\xe8\xaf\x84\xe4\xbc\xb0\xe4\xb8\xad"  /* 评估中 */
#define HRPT_ST_OK           "\xe6\xad\xa3\xe5\xb8\xb8"  /* 正常 */
#define HRPT_ST_HIGH         "\xe5\x81\x8f\xe9\xab\x98"  /* 偏高 */
#define HRPT_ST_LOW          "\xe5\x81\x8f\xe4\xbd\x8e"  /* 偏低 */
#define HRPT_ST_FAST         "\xe5\x81\x8f\xe5\xbf\xab"  /* 偏快 */
#define HRPT_ST_SLOW         "\xe5\x81\x8f\xe6\x85\xa2"  /* 偏慢 */
#define HRPT_ST_MID          "\xe4\xb8\xad\xe7\xad\x89"  /* 中等 */
#define HRPT_ST_WATCH        "\xe5\x85\xb3\xe6\xb3\xa8"  /* 关注 */
#define HRPT_ST_ABN          "\xe5\xbc\x82\xe5\xb8\xb8"  /* 异常 */
#define HRPT_ST_NA           "\xe6\x9c\xaa\xe6\xb5\x8b"  /* 未测 */
#define HRPT_ST_SINUS        "\xe7\xaa\xa6\xe6\x80\xa7"  /* 窦性 */
#define HRPT_NO_PVC          "\xe6\x97\xa0\xe6\x97\xa9\xe6\x90\x8f"  /* 无早搏 */
/* HRPT_SUM_NORMAL / HRPT_SUM_WATCH 已移到 health_report.h(与 UI 共享) */
#define HRPT_ADV_ALL_OK      "\xe5\x90\x84\xe9\xa1\xb9\xe6\x8c\x87\xe6\xa0\x87\xe5\x9d\x87\xe5\x9c\xa8\xe6\xad\xa3\xe5\xb8\xb8\xe8\x8c\x83\xe5\x9b\xb4\xef\xbc\x8c\xe8\xaf\xb7\xe7\xbb\xa7\xe7\xbb\xad\xe4\xbf\x9d\xe6\x8c\x81\xe5\x81\xa5\xe5\xba\xb7\xe7\x9a\x84\xe7\x94\x9f\xe6\xb4\xbb\xe6\x96\xb9\xe5\xbc\x8f\xe3\x80\x82"  /* 各项指标均在正常范围，请继续保持健康的生活方式。 */
#define HRPT_ADV_HR_FAST     "\xe5\xbf\x83\xe7\x8e\x87\xe5\x81\x8f\xe5\xbf\xab\xef\xbc\x8c\xe6\xb3\xa8\xe6\x84\x8f\xe4\xbc\x91\xe6\x81\xaf\xef\xbc\x8c\xe9\x81\xbf\xe5\x85\x8d\xe5\x89\xa7\xe7\x83\x88\xe8\xbf\x90\xe5\x8a\xa8\xe3\x80\x82"  /* 心率偏快，注意休息，避免剧烈运动。 */
#define HRPT_ADV_HR_SLOW     "\xe5\xbf\x83\xe7\x8e\x87\xe5\x81\x8f\xe6\x85\xa2\xef\xbc\x8c\xe5\xa6\x82\xe6\x8c\x81\xe7\xbb\xad\xe6\x88\x96\xe4\xbc\xb4\xe6\x9c\x89\xe4\xb8\x8d\xe9\x80\x82\xe8\xaf\xb7\xe5\x8f\x8a\xe6\x97\xb6\xe5\xb0\xb1\xe5\x8c\xbb\xe3\x80\x82"  /* 心率偏慢，如持续或伴有不适请及时就医。 */
#define HRPT_ADV_SPO2_LOW    "\xe8\xa1\x80\xe6\xb0\xa7\xe5\x81\x8f\xe4\xbd\x8e\xef\xbc\x8c\xe8\xaf\xb7\xe5\xae\x89\xe9\x9d\x99\xe6\xb7\xb1\xe5\x91\xbc\xe5\x90\xb8\xe5\x90\x8e\xe5\xa4\x8d\xe6\xb5\x8b\xe3\x80\x82"  /* 血氧偏低，请安静深呼吸后复测。 */
#define HRPT_ADV_SPO2_BAD    "\xe8\xa1\x80\xe6\xb0\xa7\xe6\x98\x8e\xe6\x98\xbe\xe5\x81\x8f\xe4\xbd\x8e\xef\xbc\x8c\xe5\xbb\xba\xe8\xae\xae\xe5\xb0\xbd\xe5\xbf\xab\xe5\xb0\xb1\xe5\x8c\xbb\xe6\xa3\x80\xe6\x9f\xa5\xe3\x80\x82"  /* 血氧明显偏低，建议尽快就医检查。 */
#define HRPT_ADV_ARR         "\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0\xe6\x97\xa9\xe6\x90\x8f\xe4\xbf\xa1\xe5\x8f\xb7\xef\xbc\x8c\xe5\xbb\xba\xe8\xae\xae\xe7\xbb\x93\xe5\x90\x88\xe7\x97\x87\xe7\x8a\xb6\xe5\xb0\xb1\xe5\x8c\xbb\xe8\xaf\x84\xe4\xbc\xb0\xe3\x80\x82"  /* 检测到早搏信号，建议结合症状就医评估。 */
#define HRPT_ADV_RESP        "\xe5\x91\xbc\xe5\x90\xb8\xe9\xa2\x91\xe7\x8e\x87\xe5\x81\x8f\xe7\xa6\xbb\xe5\xb8\xb8\xe6\x80\x81\xef\xbc\x8c\xe5\xb1\x9e\xe5\xae\x9e\xe9\xaa\x8c\xe6\x80\xa7\xe6\x8c\x87\xe6\xa0\x87\xef\xbc\x8c\xe8\xaf\xb7\xe6\x94\xbe\xe6\x9d\xbe\xe5\x90\x8e\xe5\xa4\x8d\xe6\xb5\x8b\xe3\x80\x82"  /* 呼吸频率偏离常态，属实验性指标，请放松后复测。 */
#define HRPT_ADV_HRV         "\xe5\xbf\x83\xe7\x8e\x87\xe5\x8f\x98\xe5\xbc\x82\xe6\x80\xa7\xe5\x81\x8f\xe4\xbd\x8e\xef\xbc\x8c\xe6\xb3\xa8\xe6\x84\x8f\xe4\xbc\x91\xe6\x81\xaf\xe4\xb8\x8e\xe5\x8e\x8b\xe5\x8a\x9b\xe8\xb0\x83\xe8\x8a\x82\xe3\x80\x82"  /* 心率变异性偏低，注意休息与压力调节。 */
#define HRPT_ADV_STRESS      "\xe5\xba\x94\xe6\xbf\x80\xe6\xb0\xb4\xe5\xb9\xb3\xe5\x81\x8f\xe9\xab\x98\xef\xbc\x8c\xe5\xbb\xba\xe8\xae\xae\xe6\x94\xbe\xe6\x9d\xbe\xe5\xbf\x83\xe6\x83\x85\xe3\x80\x81\xe8\xa7\x84\xe5\xbe\x8b\xe4\xbd\x9c\xe6\x81\xaf\xe3\x80\x82"  /* 应激水平偏高，建议放松心情、规律作息。 */
#define HRPT_ADV_GLU         "\xe8\xa1\x80\xe7\xb3\x96\xe5\x8f\x82\xe8\x80\x83\xe5\x80\xbc\xe5\x81\x8f\xe7\xa6\xbb\xe5\xb8\xb8\xe6\x80\x81\xef\xbc\x8c\xe4\xb8\xba\xe5\xae\x9e\xe9\xaa\x8c\xe6\x80\xa7\xe6\x8c\x87\xe6\xa0\x87\xe4\xbb\x85\xe4\xbe\x9b\xe5\x8f\x82\xe8\x80\x83\xe3\x80\x82"  /* 血糖参考值偏离常态，为实验性指标仅供参考。 */
#define HRPT_ADV_COLLECT     "\xe6\x95\xb0\xe6\x8d\xae\xe9\x87\x87\xe9\x9b\x86\xe4\xb8\xad\xef\xbc\x8c\xe8\xaf\xb7\xe5\xb0\x86\xe6\x89\x8b\xe6\x8c\x87\xe7\xa8\xb3\xe5\xae\x9a\xe6\x94\xbe\xe7\xbd\xae\xe4\xba\x8e\xe4\xbc\xa0\xe6\x84\x9f\xe5\x99\xa8\xe3\x80\x82"  /* 数据采集中，请将手指稳定放置于传感器。 */
/* HRPT_DISCLAIMER 已移到 health_report.h(与 UI 共享) */

/* 单项参考范围(静态)，与 UI 卡片副标题一一对应 */
static const char *const s_ref[HRPT_M_N] = {
    "60-100",       /* HR   bpm */
    "95-100",       /* SpO2 %   */
    HRPT_NO_PVC,    /* 心律 无早搏 */
    "12-20",        /* 呼吸 次/分 */
    "20-120",       /* HRV  SDNN ms */
    "0-40",         /* 应激 越低越好 */
    "3.9-6.1",      /* 血糖 mmol/L */
};

const char *health_report_ref_text(int metric)
{
    if (metric < 0 || metric >= HRPT_M_N) return "";
    return s_ref[metric];
}

/* 记录心率方向，供选建议时区分偏快/偏慢 */
#define HR_DIR_NONE   0
#define HR_DIR_FAST   1
#define HR_DIR_SLOW  (-1)

static void eval_hr(int hr, int valid, hrpt_eval_t *e, int *dir)
{
    *dir = HR_DIR_NONE;
    if (!valid) { e->level = HRPT_LV_NA; e->status_text = HRPT_ST_NA; return; }
    if (hr < 40 || hr > 140) {
        e->level = HRPT_LV_DANGER;
        e->status_text = (hr < 40) ? HRPT_ST_SLOW : HRPT_ST_FAST;
        *dir = (hr < 40) ? HR_DIR_SLOW : HR_DIR_FAST;
    } else if (hr < 50) {
        e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_SLOW; *dir = HR_DIR_SLOW;
    } else if (hr > 100) {
        e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_FAST; *dir = HR_DIR_FAST;
    } else {
        e->level = HRPT_LV_OK; e->status_text = HRPT_ST_OK;
    }
}

static void eval_spo2(float spo2, int valid, hrpt_eval_t *e)
{
    if (!valid) { e->level = HRPT_LV_NA; e->status_text = HRPT_ST_NA; return; }
    if (spo2 < 90.0f) { e->level = HRPT_LV_DANGER; e->status_text = HRPT_ST_LOW; }
    else if (spo2 < 95.0f) { e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_LOW; }
    else { e->level = HRPT_LV_OK; e->status_text = HRPT_ST_OK; }
}

static void eval_rhythm(int valid, unsigned beats, unsigned pvc, hrpt_eval_t *e)
{
    if (!valid || beats == 0) { e->level = HRPT_LV_NA; e->status_text = HRPT_ST_NA; return; }
    if (pvc > 6)      { e->level = HRPT_LV_DANGER; e->status_text = HRPT_ST_ABN; }
    else if (pvc > 0) { e->level = HRPT_LV_WARN;   e->status_text = HRPT_ST_WATCH; }
    else              { e->level = HRPT_LV_OK;     e->status_text = HRPT_ST_SINUS; }
}

/* 呼吸率：实验性，异常也只到 WARN */
static void eval_resp(float rr, int valid, hrpt_eval_t *e)
{
    if (!valid) { e->level = HRPT_LV_NA; e->status_text = HRPT_ST_NA; return; }
    if (rr < 12.0f)      { e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_LOW; }
    else if (rr > 20.0f) { e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_HIGH; }
    else                 { e->level = HRPT_LV_OK;   e->status_text = HRPT_ST_OK; }
}

static void eval_hrv(float sdnn, int valid, hrpt_eval_t *e)
{
    if (!valid) { e->level = HRPT_LV_NA; e->status_text = HRPT_ST_NA; return; }
    if (sdnn < 20.0f) { e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_LOW; }
    else              { e->level = HRPT_LV_OK;   e->status_text = HRPT_ST_OK; }
}

/* 应激(GSR)：<40 正常 / 40-70 中等 / >70 偏高，均不升到就医级 */
static void eval_stress(float s, int valid, hrpt_eval_t *e, int *high)
{
    *high = 0;
    if (!valid) { e->level = HRPT_LV_NA; e->status_text = HRPT_ST_NA; return; }
    if (s > 70.0f)      { e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_HIGH; *high = 1; }
    else if (s >= 40.0f){ e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_MID; }
    else                { e->level = HRPT_LV_OK;   e->status_text = HRPT_ST_OK; }
}

/* 血糖：实验性，异常只到 WARN */
static void eval_glucose(float g, int valid, hrpt_eval_t *e)
{
    if (!valid) { e->level = HRPT_LV_NA; e->status_text = HRPT_ST_NA; return; }
    if (g < 3.9f)      { e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_LOW; }
    else if (g > 6.1f) { e->level = HRPT_LV_WARN; e->status_text = HRPT_ST_HIGH; }
    else               { e->level = HRPT_LV_OK;   e->status_text = HRPT_ST_OK; }
}

static void push_advice(health_report_t *r, const char *a)
{
    if (r->advice_count < HRPT_MAX_ADVICE)
        r->advice[r->advice_count++] = a;
}

void health_report_build(const health_report_input_t *in, health_report_t *out)
{
    int hr_dir = HR_DIR_NONE;
    int stress_high = 0;
    int i;

    if (out == NULL) return;

    /* --- 逐项判定 --- */
    eval_hr(in->heart_rate, in->hr_valid, &out->m[HRPT_M_HR], &hr_dir);
    eval_spo2(in->spo2, in->spo2_valid, &out->m[HRPT_M_SPO2]);
    eval_rhythm(in->arr_valid, in->arr_beat_count, in->arr_pvc_per_min,
                &out->m[HRPT_M_RHYTHM]);
    eval_resp(in->resp_rate_bpm, in->resp_valid, &out->m[HRPT_M_RESP]);
    eval_hrv(in->hrv_sdnn_ms, in->hrv_valid, &out->m[HRPT_M_HRV]);
    eval_stress(in->gsr_stress, in->gsr_valid, &out->m[HRPT_M_STRESS], &stress_high);
    eval_glucose(in->glucose_mmol, in->glucose_valid, &out->m[HRPT_M_GLU]);

    /* --- 正常/需留意计数 --- */
    out->normal_count = 0;
    out->watch_count = 0;
    for (i = 0; i < HRPT_M_N; i++) {
        if (out->m[i].level == HRPT_LV_OK)          out->normal_count++;
        else if (out->m[i].level == HRPT_LV_WARN ||
                 out->m[i].level == HRPT_LV_DANGER) out->watch_count++;
    }

    /* --- 综合评级(沿用 health_score 分数) --- */
    out->score = in->health_score;
    out->score_valid = in->health_score_valid;
    if (!in->health_score_valid) {
        out->grade_text = HRPT_GRADE_PENDING; out->grade_level = HRPT_LV_NA;
    } else if (in->health_score >= 90) {
        out->grade_text = HRPT_GRADE_EXCELLENT; out->grade_level = HRPT_LV_OK;
    } else if (in->health_score >= 80) {
        out->grade_text = HRPT_GRADE_GOOD; out->grade_level = HRPT_LV_OK;
    } else if (in->health_score >= 60) {
        out->grade_text = HRPT_GRADE_FAIR; out->grade_level = HRPT_LV_WARN;
    } else {
        out->grade_text = HRPT_GRADE_ATTENTION; out->grade_level = HRPT_LV_DANGER;
    }

    /* --- 建议选择(按临床严重度排序，最多 HRPT_MAX_ADVICE 条) --- */
    out->advice_count = 0;

    if (out->m[HRPT_M_RHYTHM].level >= HRPT_LV_WARN)
        push_advice(out, HRPT_ADV_ARR);

    if (out->m[HRPT_M_SPO2].level == HRPT_LV_DANGER)
        push_advice(out, HRPT_ADV_SPO2_BAD);
    else if (out->m[HRPT_M_SPO2].level == HRPT_LV_WARN)
        push_advice(out, HRPT_ADV_SPO2_LOW);

    if (hr_dir == HR_DIR_FAST)      push_advice(out, HRPT_ADV_HR_FAST);
    else if (hr_dir == HR_DIR_SLOW) push_advice(out, HRPT_ADV_HR_SLOW);

    if (stress_high)                          push_advice(out, HRPT_ADV_STRESS);
    if (out->m[HRPT_M_HRV].level == HRPT_LV_WARN) push_advice(out, HRPT_ADV_HRV);
    if (out->m[HRPT_M_RESP].level >= HRPT_LV_WARN) push_advice(out, HRPT_ADV_RESP);
    if (out->m[HRPT_M_GLU].level  >= HRPT_LV_WARN) push_advice(out, HRPT_ADV_GLU);

    if (out->advice_count == 0)
        push_advice(out, in->health_score_valid ? HRPT_ADV_ALL_OK : HRPT_ADV_COLLECT);

    out->disclaimer = HRPT_DISCLAIMER;
}
