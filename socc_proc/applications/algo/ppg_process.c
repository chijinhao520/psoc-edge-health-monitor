/*
 * Derived from Maxim MAXREFDES117 reference code.
 * Original notice: LICENSES/Maxim-Integrated.txt
 * Project modifications: RT-Thread port, timing and validity fixes.
 */
/*
 * PPG 信号处理算法实现
 *
 * 移植自 SocChina 2025 博流赛道参考项目（BL616 版本）
 * 纯数学算法；采样时钟、边界与比值计算已经修正
 *
 * 移植变更：
 *   - 删除 bflb_mtimer.h / bflb_gpio.h（无硬件依赖）
 *   - 修复时间口径、数组边界、比值缩放及整数溢出
 */

#include "ppg_process.h"
#include <stdint.h>

/* ======================== 查找表（只读常量） ======================== */

/*
 * 汉明窗系数（5点），用于 PPG 差分信号的频谱平滑
 * 原始值：512 * hamming(5) = [41, 276, 512, 276, 41]
 * 分母（系数之和）= 1146，使用时除以 1146 完成归一化
 */
static const uint16_t auw_hamm[5] = { 41, 276, 512, 276, 41 };

/*
 * SpO2 查找表（184项）
 * 由公式 -45.060 * ratio^2 + 30.354 * ratio + 94.845 预计算得出
 * 索引为 AC/DC 比值（乘以缩放系数后的整数），值为对应的 SpO2 百分比
 */
static const uint8_t uch_spo2_table[184] = {
    95, 95, 95, 96, 96, 96, 97, 97, 97, 97, 97, 98, 98, 98, 98, 98,
    99, 99, 99, 99, 99, 99, 99, 99, 100, 100, 100, 100, 100, 100, 100, 100,
    100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 99, 99, 99, 99,
    99, 99, 99, 99, 98, 98, 98, 98, 98, 98, 97, 97, 97, 97, 96, 96,
    96, 96, 95, 95, 95, 94, 94, 94, 93, 93, 93, 92, 92, 92, 91, 91,
    90, 90, 89, 89, 89, 88, 88, 87, 87, 86, 86, 85, 85, 84, 84, 83,
    82, 82, 81, 81, 80, 80, 79, 78, 78, 77, 76, 76, 75, 74, 74, 73,
    72, 72, 71, 70, 69, 69, 68, 67, 66, 66, 65, 64, 63, 62, 62, 61,
    60, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46,
    45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 31, 30, 29,
    28, 27, 26, 25, 23, 22, 21, 20, 19, 17, 16, 15, 14, 12, 11, 10,
    9, 7, 6, 5, 3, 2, 1, 0
};

/* ======================== 算法内部工作缓冲区（文件作用域静态） ======================== */

/* 静态工作区：仅由 PPG 线程串行调用，非可重入。 */
static int32_t an_dx[BUFFER_SIZE - MA4_SIZE];  /* 红外一阶差分（心率算法） */
static int32_t an_x[BUFFER_SIZE];              /* 红外工作数组 */
static int32_t an_y[BUFFER_SIZE];              /* 红光工作数组 */

/* ======================== 辅助函数实现 ======================== */

/**
 * @brief 升序排序（插入排序）
 */
void maxim_sort_ascend(int32_t *pn_x, int32_t n_size)
{
    int32_t i, j, n_temp;
    for (i = 1; i < n_size; i++)
    {
        n_temp = pn_x[i];
        for (j = i; j > 0 && n_temp < pn_x[j - 1]; j--)
            pn_x[j] = pn_x[j - 1];
        pn_x[j] = n_temp;
    }
}

/**
 * @brief 按 pn_x 中的值对索引数组 pn_indx 降序排序（插入排序）
 */
void maxim_sort_indices_descend(int32_t *pn_x, int32_t *pn_indx, int32_t n_size)
{
    int32_t i, j, n_temp;
    for (i = 1; i < n_size; i++)
    {
        n_temp = pn_indx[i];
        for (j = i; j > 0 && pn_x[n_temp] > pn_x[pn_indx[j - 1]]; j--)
            pn_indx[j] = pn_indx[j - 1];
        pn_indx[j] = n_temp;
    }
}

/**
 * @brief 找出所有高于 n_min_height 的波峰位置
 *
 * 遍历信号，识别局部极大值（含平顶波峰）。
 * 最多记录 15 个波峰位置到 pn_locs[]。
 */
void maxim_peaks_above_min_height(int32_t *pn_locs, int32_t *pn_npks,
                                   int32_t *pn_x, int32_t n_size, int32_t n_min_height)
{
    int32_t i = 1, n_width;
    *pn_npks = 0;

    while (i < n_size - 1)
    {
        if (pn_x[i] > n_min_height && pn_x[i] > pn_x[i - 1])
        {
            /* 找到波峰左边缘，处理平顶（多点相等）情况 */
            n_width = 1;
            while (i + n_width < n_size && pn_x[i] == pn_x[i + n_width])
                n_width++;

            if (i + n_width < n_size && pn_x[i] > pn_x[i + n_width] && (*pn_npks) < 15)
            {
                /* 确认为有效波峰，记录左边缘位置 */
                pn_locs[(*pn_npks)++] = i;
                i += n_width + 1;
            }
            else
            {
                i += n_width;
            }
        }
        else
        {
            i++;
        }
    }
}

/**
 * @brief 删除距离过近的波峰，保留幅值较大者
 *
 * 先按幅值降序排列，逐一保留与已选波峰距离 > n_min_distance 的候选，
 * 最后再按位置升序重排，恢复时间顺序。
 */
void maxim_remove_close_peaks(int32_t *pn_locs, int32_t *pn_npks,
                               int32_t *pn_x, int32_t n_min_distance)
{
    int32_t i, j, n_old_npks, n_dist;

    /* 按幅值降序排列波峰索引（优先保留大波峰） */
    maxim_sort_indices_descend(pn_x, pn_locs, *pn_npks);

    for (i = -1; i < *pn_npks; i++)
    {
        n_old_npks = *pn_npks;
        *pn_npks = i + 1;
        for (j = i + 1; j < n_old_npks; j++)
        {
            /* lag-zero 自相关峰在索引 -1 处，因此 i==-1 时基准位置取 -1 */
            n_dist = pn_locs[j] - (i == -1 ? -1 : pn_locs[i]);
            if (n_dist > n_min_distance || n_dist < -n_min_distance)
                pn_locs[(*pn_npks)++] = pn_locs[j];
        }
    }

    /* 恢复升序（时间顺序） */
    maxim_sort_ascend(pn_locs, *pn_npks);
}

/**
 * @brief 综合波峰检测：找高于阈值的峰 → 删除过近的峰 → 限制最大数量
 */
void maxim_find_peaks(int32_t *pn_locs, int32_t *pn_npks,
                      int32_t *pn_x, int32_t n_size,
                      int32_t n_min_height, int32_t n_min_distance, int32_t n_max_num)
{
    maxim_peaks_above_min_height(pn_locs, pn_npks, pn_x, n_size, n_min_height);
    maxim_remove_close_peaks(pn_locs, pn_npks, pn_x, n_min_distance);
    *pn_npks = min(*pn_npks, n_max_num);  /* 保留前 n_max_num 个波峰 */
}

/* ======================== 主要算法实现 ======================== */

/**
 * @brief 计算心率和血氧饱和度（SpO2）
 *
 * 算法流程：
 *   Step1: 对红外信号去直流（减均值），得到 AC 分量
 *   Step2: 4点移动平均滤波，平滑高频噪声
 *   Step3: 一阶差分，突出波峰位置
 *   Step4: 差分结果再做 2点移动平均
 *   Step5: 汉明窗加权，抑制频谱泄露
 *   Step6: 峰值检测 → 计算平均心跳间隔 → 心率 = 6000 / 间隔（100Hz下）
 *   Step7: 在相邻波谷之间计算 RED/IR 的 AC/DC 比值
 *   Step8: 查表 uch_spo2_table 得到 SpO2
 */
/* fs 是连续 ADC 样本的传感器时钟，与 FIFO 读取速度无关。 */
static int32_t hr_min_peak_dist(uint32_t fs)
{
    uint32_t f = (fs > 0u) ? fs : (uint32_t)FS;
    int32_t  d = (int32_t)(f * (uint32_t)HR_MIN_PEAK_DIST_MS / 1000u);

    return (d < 2) ? 2 : d;
}

void maxim_heart_rate_and_oxygen_saturation(uint32_t *pun_ir_buffer,
                                             int32_t   n_ir_buffer_length,
                                             uint32_t *pun_red_buffer,
                                             int32_t  *pn_spo2,
                                             int8_t   *pch_spo2_valid,
                                             int32_t  *pn_heart_rate,
                                             int8_t   *pch_hr_valid,
                                             uint32_t  fs)
{
    uint32_t un_ir_mean, un_only_once;
    int32_t  k, n_i_ratio_count;
    int32_t  i, s, m, n_exact_ir_valley_locs_count, n_middle_idx;
    int32_t  n_th1, n_npks, n_c_min;
    int32_t  an_ir_valley_locs[15];
    int32_t  an_exact_ir_valley_locs[15];
    int32_t  an_dx_peak_locs[15];
    int32_t  n_peak_interval_sum;
    int32_t  n_y_ac, n_x_ac;
    int32_t  n_spo2_calc;
    int32_t  n_y_dc_max, n_x_dc_max;
    int32_t  n_y_dc_max_idx = 0, n_x_dc_max_idx = 0;
    int32_t  an_ratio[5], n_ratio_average;
    int64_t  n_nume, n_denom;

    /* 先清输出；固定窗口与 18 位 ADC 契约不满足时禁止读取。 */
    if (pn_spo2) *pn_spo2 = -999;
    if (pn_heart_rate) *pn_heart_rate = -999;
    if (pch_spo2_valid) *pch_spo2_valid = 0;
    if (pch_hr_valid) *pch_hr_valid = 0;
    if (!pn_spo2 || !pn_heart_rate || !pch_spo2_valid || !pch_hr_valid ||
        !pun_ir_buffer || !pun_red_buffer || n_ir_buffer_length != BUFFER_SIZE ||
        fs == 0 || fs > 1000) return;
    for (k = 0; k < BUFFER_SIZE; k++)
        if (pun_ir_buffer[k] > 0x3FFFFu || pun_red_buffer[k] > 0x3FFFFu) return;

    /* ---------- Step1: 去直流 ---------- */
    /* 计算红外信号均值，用于去除直流偏置 */
    un_ir_mean = 0;
    for (k = 0; k < n_ir_buffer_length; k++)
        un_ir_mean += pun_ir_buffer[k];
    un_ir_mean = un_ir_mean / n_ir_buffer_length;

    /* 减去均值，得到以 0 为基线的 AC 分量 */
    for (k = 0; k < n_ir_buffer_length; k++)
        an_x[k] = (int32_t)pun_ir_buffer[k] - (int32_t)un_ir_mean;

    /* ---------- Step2: 4点移动平均滤波 ---------- */
    for (k = 0; k < BUFFER_SIZE - MA4_SIZE; k++)
    {
        n_denom = an_x[k] + an_x[k + 1] + an_x[k + 2] + an_x[k + 3];
        an_x[k] = (int32_t)(n_denom / 4);
    }

    /* ---------- Step3: 一阶差分（突出波峰边沿） ---------- */
    for (k = 0; k < BUFFER_SIZE - MA4_SIZE - 1; k++)
        an_dx[k] = an_x[k + 1] - an_x[k];

    /* ---------- Step4: 差分后再做 2点移动平均 ---------- */
    for (k = 0; k < BUFFER_SIZE - MA4_SIZE - 2; k++)
        an_dx[k] = (an_dx[k] + an_dx[k + 1]) / 2;

    /* ---------- Step5: 汉明窗加权（抑制频谱泄露） ---------- */
    /* 对差分信号的每个位置，用 5点汉明窗做加权求和，再除以系数之和 1146 */
    for (i = 0; i < BUFFER_SIZE - HAMMING_SIZE - MA4_SIZE - 2; i++)
    {
        s = 0;
        for (k = i; k < i + HAMMING_SIZE; k++)
            s -= an_dx[k] * auw_hamm[k - i];
        an_dx[i] = s / (int32_t)1146;
    }

    /* ---------- Step6: 峰值检测 → 心率 ---------- */
    /* 计算差分信号的平均绝对值作为自适应阈值 */
    n_th1 = 0;
    for (k = 0; k < BUFFER_SIZE - HAMMING_SIZE - MA4_SIZE - 2; k++)
        n_th1 += (an_dx[k] > 0) ? an_dx[k] : (int32_t)0 - an_dx[k];
    n_th1 = n_th1 / (BUFFER_SIZE - HAMMING_SIZE - MA4_SIZE - 2);

    /* 340ms 峰距用于抑制重搏波；仅处理经过全部滤波的有效前缀。 */
    maxim_find_peaks(an_dx_peak_locs, &n_npks, an_dx,
                     BUFFER_SIZE - HAMMING_SIZE - MA4_SIZE - 2, n_th1, hr_min_peak_dist(fs), 5);

    /* 计算平均波峰间隔，推算心率：100Hz 下每分钟 6000 个采样点 */
    n_peak_interval_sum = 0;
    if (n_npks >= 2)
    {
        for (k = 1; k < n_npks; k++)
            n_peak_interval_sum += (an_dx_peak_locs[k] - an_dx_peak_locs[k - 1]);
        *pn_heart_rate = (int32_t)((60u * fs * (uint32_t)(n_npks - 1) +
                            (uint32_t)n_peak_interval_sum / 2u) / (uint32_t)n_peak_interval_sum);
        *pch_hr_valid  = 1;
    }
    else
    {
        *pn_heart_rate = -999;  /* 波峰不足，无法计算 */
        *pch_hr_valid  = 0;
    }

    /* ---------- 波谷位置修正 ---------- */
    /* 差分波峰对应原始波形上升最陡处，加 HAMMING_SIZE/2 偏移还原到实际波谷 */
    for (k = 0; k < n_npks; k++)
        an_ir_valley_locs[k] = an_dx_peak_locs[k] + HAMMING_SIZE / 2;

    /* ---------- Step7: 计算 SpO2 所需的 AC/DC 比值 ---------- */
    /* 将原始红外和红光数据载入工作数组 */
    for (k = 0; k < n_ir_buffer_length; k++)
    {
        an_x[k] = pun_ir_buffer[k];   /* 红外（IR） */
        an_y[k] = pun_red_buffer[k];  /* 红光（RED） */
    }

    /* 在每个估计波谷位置周围 ±5 点内精确查找最小值，减小 SpO2 误差 */
    n_exact_ir_valley_locs_count = 0;
    for (k = 0; k < n_npks; k++)
    {
        un_only_once = 1;
        m = an_ir_valley_locs[k];
        n_c_min = 16777216;  /* 2^24，初始化为大值 */
        if (m + 5 < BUFFER_SIZE - HAMMING_SIZE && m - 5 > 0)
        {
            for (i = m - 5; i < m + 5; i++)
            {
                if (an_x[i] < n_c_min)
                {
                    if (un_only_once > 0)
                        un_only_once = 0;
                    n_c_min = an_x[i];
                    an_exact_ir_valley_locs[n_exact_ir_valley_locs_count] = i;
                }
            }
            if (un_only_once == 0)
                n_exact_ir_valley_locs_count++;
        }
    }

    /* 精确波谷不足 2 个，无法计算 SpO2 */
    if (n_exact_ir_valley_locs_count < 2)
    {
        *pn_spo2        = -999;
        *pch_spo2_valid = 0;
        return;
    }

    /* 对红外和红光再做 4点移动平均，用于 DC 分量估算 */
    for (k = 0; k < BUFFER_SIZE - MA4_SIZE; k++)
    {
        an_x[k] = (an_x[k] + an_x[k + 1] + an_x[k + 2] + an_x[k + 3]) / (int32_t)4;
        an_y[k] = (an_y[k] + an_y[k + 1] + an_y[k + 2] + an_y[k + 3]) / (int32_t)4;
    }

    /* 校验所有波谷位置在有效范围内 */
    for (k = 0; k < n_exact_ir_valley_locs_count; k++)
    {
        if (an_exact_ir_valley_locs[k] >= BUFFER_SIZE - MA4_SIZE)
        {
            *pn_spo2        = -999;
            *pch_spo2_valid = 0;
            return;
        }
    }

    /* 在相邻两个波谷之间，计算 RED/IR 的 AC/DC 比值 */
    n_ratio_average  = 0;
    n_i_ratio_count  = 0;
    for (k = 0; k < 5; k++) an_ratio[k] = 0;

    for (k = 0; k < n_exact_ir_valley_locs_count - 1; k++)
    {
        n_y_dc_max = -16777216;
        n_x_dc_max = -16777216;

        if (an_exact_ir_valley_locs[k + 1] - an_exact_ir_valley_locs[k] > 10)
        {
            /* 找两波谷之间的 DC 最大值（近似为心动周期内的 DC 水平） */
            for (i = an_exact_ir_valley_locs[k]; i < an_exact_ir_valley_locs[k + 1]; i++)
            {
                if (an_x[i] > n_x_dc_max) { n_x_dc_max = an_x[i]; n_x_dc_max_idx = i; }
                if (an_y[i] > n_y_dc_max) { n_y_dc_max = an_y[i]; n_y_dc_max_idx = i; }
            }

            /* 线性插值去除 DC，得到 AC 分量 */
            /* RED AC */
            n_y_ac = (an_y[an_exact_ir_valley_locs[k + 1]] - an_y[an_exact_ir_valley_locs[k]])
                   * (n_y_dc_max_idx - an_exact_ir_valley_locs[k]);
            n_y_ac = an_y[an_exact_ir_valley_locs[k]]
                   + n_y_ac / (an_exact_ir_valley_locs[k + 1] - an_exact_ir_valley_locs[k]);
            n_y_ac = an_y[n_y_dc_max_idx] - n_y_ac;

            /* IR AC */
            n_x_ac = (an_x[an_exact_ir_valley_locs[k + 1]] - an_x[an_exact_ir_valley_locs[k]])
                   * (n_x_dc_max_idx - an_exact_ir_valley_locs[k]);
            n_x_ac = an_x[an_exact_ir_valley_locs[k]]
                   + n_x_ac / (an_exact_ir_valley_locs[k + 1] - an_exact_ir_valley_locs[k]);
            n_x_ac = an_x[n_x_dc_max_idx] - n_x_ac;

            /* ratio = (RED_AC / RED_DC) / (IR_AC / IR_DC)，64 位保留乘积。 */
            n_nume  = (int64_t)n_y_ac * n_x_dc_max;
            n_denom = (int64_t)n_x_ac * n_y_dc_max;

            if (n_denom > 0 && n_i_ratio_count < 5 && n_nume > 0)
            {
                /* 表索引 R*100；64 位乘法避免正常 ADC 幅度就溢出。 */
                int64_t ratio = (n_nume * 100) / n_denom;
                if (ratio > 2 && ratio < 184)
                    an_ratio[n_i_ratio_count++] = (int32_t)ratio;
            }
        }
    }

    /* 取中位数 ratio，提高鲁棒性 */
    maxim_sort_ascend(an_ratio, n_i_ratio_count);
    n_middle_idx = n_i_ratio_count / 2;

    if (n_i_ratio_count == 0) return;
    if ((n_i_ratio_count % 2) == 0)
        n_ratio_average = (an_ratio[n_middle_idx - 1] + an_ratio[n_middle_idx]) / 2;
    else
        n_ratio_average = an_ratio[n_middle_idx];

    /* ---------- Step8: 查表得到 SpO2 ---------- */
    if (n_ratio_average > 2 && n_ratio_average < 184)
    {
        n_spo2_calc     = uch_spo2_table[n_ratio_average];
        *pn_spo2        = n_spo2_calc;
        *pch_spo2_valid = 1;
    }
    else
    {
        *pn_spo2        = -999;  /* ratio 超出查表范围，结果无效 */
        *pch_spo2_valid = 0;
    }
}

/* 单点 PPG 的二阶差分间距不是 PWTT，无法给出血流速度。
 * 保留旧 ABI，恒返回 0（不可用）；下游不得将其标为有效测量。 */
float maxim_blood_flow_rate(uint32_t *pun_ir_buffer, int32_t n_ir_buffer_length)
{
    (void)pun_ir_buffer;
    (void)n_ir_buffer_length;
    return 0.0f;
}
