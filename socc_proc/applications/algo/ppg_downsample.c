/*
 * PPG 原始波形下采样实现
 *
 * 500 点 → 4 点平均池化 → 125 点 → 尾部复制 → 128 点 → 归一化 [0,1]
 */

#include "ppg_downsample.h"

void ppg_downsample(const uint32_t *red, const uint32_t *ir, int n,
                    float out[PPG_DS_LEN][PPG_DS_CHANNELS])
{
    int pooled_len = n / PPG_DS_POOL; /* 500/4 = 125 */
    int i, j;
    float sum;
    float min_r, max_r, min_i, max_i, range;

    if (pooled_len > PPG_DS_LEN)
        pooled_len = PPG_DS_LEN;

    /* Step 1: 4 点平均池化 */
    for (i = 0; i < pooled_len; i++)
    {
        sum = 0.0f;
        for (j = 0; j < PPG_DS_POOL; j++)
            sum += (float)red[i * PPG_DS_POOL + j];
        out[i][0] = sum / PPG_DS_POOL;

        sum = 0.0f;
        for (j = 0; j < PPG_DS_POOL; j++)
            sum += (float)ir[i * PPG_DS_POOL + j];
        out[i][1] = sum / PPG_DS_POOL;
    }

    /* Step 2: 尾部复制填充到 128 点 */
    for (i = pooled_len; i < PPG_DS_LEN; i++)
    {
        out[i][0] = out[pooled_len - 1][0];
        out[i][1] = out[pooled_len - 1][1];
    }

    /* Step 3: 通道内 min-max 归一化 → [0, 1] */
    min_r = max_r = out[0][0];
    min_i = max_i = out[0][1];
    for (i = 1; i < PPG_DS_LEN; i++)
    {
        if (out[i][0] < min_r) min_r = out[i][0];
        if (out[i][0] > max_r) max_r = out[i][0];
        if (out[i][1] < min_i) min_i = out[i][1];
        if (out[i][1] > max_i) max_i = out[i][1];
    }

    range = max_r - min_r;
    if (range < 1.0f) range = 1.0f; /* 全零/常值信号防除零 */
    for (i = 0; i < PPG_DS_LEN; i++)
        out[i][0] = (out[i][0] - min_r) / range;

    range = max_i - min_i;
    if (range < 1.0f) range = 1.0f;
    for (i = 0; i < PPG_DS_LEN; i++)
        out[i][1] = (out[i][1] - min_i) / range;
}
