/*
 * GSR 实时处理——实现
 */

#include "gsr_process.h"
#include <string.h>
#include <math.h>

void gsr_state_reset(gsr_state_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
}

int gsr_process_sample(gsr_state_t *st, float raw_mv, float *stress_index_out)
{
    int i;

    if (stress_index_out) *stress_index_out = 0.0f;
    if (!st || !stress_index_out) return 0;
    /* 当前单端 ADS1015 ±2.048V 配置：零码/负值/正饱和无法形成可信基线。 */
    if (!isfinite(raw_mv) || raw_mv <= 0.0f || raw_mv >= 2047.0f)
    {
        gsr_state_reset(st);
        return 0;
    }

    /* EMA 平滑：第一个样本直接作为初值，避免冷启动偏差 */
    if (st->sample_count == 0)
        st->ema_mv = raw_mv;
    else
        st->ema_mv = GSR_EMA_ALPHA * raw_mv + (1.0f - GSR_EMA_ALPHA) * st->ema_mv;
    if (st->sample_count < UINT32_MAX) st->sample_count++;

    /* 短期变异滑窗记录平滑后的值 */
    st->win_buf[st->win_idx] = st->ema_mv;
    st->win_idx = (st->win_idx + 1) % GSR_WIN_LEN;
    if (st->win_idx == 0)
        st->win_filled = 1;

    /* 学习期：积累够 GSR_LEARN_SAMPLES 个样本后，把当前 EMA 锁定为个人基线 */
    if (!st->learned)
    {
        if (st->sample_count >= GSR_LEARN_SAMPLES)
        {
            st->baseline_mv = st->ema_mv;
            st->learned = 1;
        }
        else
        {
            *stress_index_out = 0.0f;
            return 0;
        }
    }

    /* 短期变异：滑窗内标准差（皮肤电导响应 SCR 表现为短时脉冲状波动） */
    int n = st->win_filled ? GSR_WIN_LEN : st->win_idx;
    if (n < 1) n = 1;

    float mean = 0.0f;
    for (i = 0; i < n; i++)
        mean += st->win_buf[i];
    mean /= (float)n;

    float var = 0.0f;
    for (i = 0; i < n; i++)
    {
        float d = st->win_buf[i] - mean;
        var += d * d;
    }
    var /= (float)n;
    float std_mv = sqrtf(var);

    /*
     * 长期偏离基线：取绝对值而非有符号差——电路方向（电压升高对应皮肤电导
     * 升高还是降低）取决于具体接线，未标定前不假设符号，只用偏离幅度。
     */
    float baseline_abs = (st->baseline_mv != 0.0f) ? fabsf(st->baseline_mv) : 1.0f;
    float drift_ratio = fabsf(st->ema_mv - st->baseline_mv) / baseline_abs;

    /*
     * 评分阈值（5mV / 5% / 15%）是现场标定前的初始猜测，不是严格推导值，
     * 上板实测应激场景（如手指用力捏一下）后应据实调整。
     */
    float score = 0.0f;
    if (std_mv > 5.0f)         score += 40.0f;
    if (drift_ratio > 0.05f)   score += 30.0f;
    if (drift_ratio > 0.15f)   score += 30.0f;
    if (score > 100.0f) score = 100.0f;

    *stress_index_out = score;
    return 1;
}
