/*
 * PPG 主通道实时峰检测——实现
 */

#include "ppg_peak_stream.h"
#include <string.h>

void ppg_peak_state_reset(ppg_peak_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

int ppg_peak_process_sample(ppg_peak_state_t *st, uint32_t raw_ir)
{
    if (!st->have_prev_raw)
    {
        st->prev_raw = (int32_t)raw_ir;
        st->have_prev_raw = 1;
        return 0;
    }

    float diff = (float)((int32_t)raw_ir - st->prev_raw);
    st->prev_raw = (int32_t)raw_ir;

    /* 一阶差分平滑(EMA)，降低采样噪声导致的假峰 */
    if (!st->initialized)
    {
        st->diff_ema = diff;
        st->initialized = 1;
    }
    else
    {
        st->diff_ema = PPG_PEAK_DIFF_EMA_ALPHA * diff
                     + (1.0f - PPG_PEAK_DIFF_EMA_ALPHA) * st->diff_ema;
    }

    st->samples_since_peak++;

    int is_peak = 0;

    if (st->have_diff_hist)
    {
        /* 局部极大：diff_prev 比前后都大，过了不应期，且超过自适应阈值 */
        if (st->diff_prev > st->diff_pprev && st->diff_prev >= st->diff_ema &&
            st->diff_prev > st->thr_ema &&
            st->samples_since_peak > PPG_PEAK_REFRACTORY_SAMPLES)
        {
            is_peak = 1;
            st->samples_since_peak = 0;
            /* 阈值包络：慢速跟踪最近峰值幅度的一半，作为下次检测的门槛 */
            st->thr_ema = PPG_PEAK_THR_EMA_ALPHA * (0.5f * st->diff_prev)
                        + (1.0f - PPG_PEAK_THR_EMA_ALPHA) * st->thr_ema;
        }
    }

    st->diff_pprev = st->diff_prev;
    st->diff_prev  = st->diff_ema;
    st->have_diff_hist = 1;

    return is_peak;
}
