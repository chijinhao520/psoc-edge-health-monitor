/*
 * Derived from Maxim MAXREFDES117 reference code.
 * Original notice: LICENSES/Maxim-Integrated.txt
 * Project modifications: RT-Thread port, timing and validity fixes.
 */
/*
 * PPG 信号处理算法头文件
 *
 * 移植自 SocChina 2025 博流赛道参考项目（BL616 版本）
 * 纯数学算法；固定窗口，非可重入
 *
 * 使用流程：
 *   1. 以 100Hz 采样率调用 max30102_read_fifo()，积累 500 个点（5秒）
 *   2. 调用 maxim_heart_rate_and_oxygen_saturation() 得到心率 + SpO2
 *   3. 血流速度路径未验证，兼容函数恒返回 0，不可用于健康测量
 */

#ifndef __PPG_PROCESS_H__
#define __PPG_PROCESS_H__

#include <stdint.h>

/* ======================== 采样参数（禁止随意修改） ======================== */

/* MAX30102 SPO2_CONFIG=0x27，FIFO 无平均，传感器输出固定 100Hz。 */
#define FS 100
#define BUFFER_SIZE (FS * 5)
#define HR_FIFO_SIZE 7
#define MA4_SIZE 4
#define HAMMING_SIZE 5
/* 340ms 最小峰距，按传入的样本时钟折算；变更后运行 tools/hr_sil。 */
#ifndef HR_MIN_PEAK_DIST_MS
#define HR_MIN_PEAK_DIST_MS  340
#endif

#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif

/* ======================== 主要算法函数 ======================== */

/**
 * @brief 计算心率和血氧饱和度（SpO2）
 *
 * 算法原理：
 *   - 对红外 PPG 信号做去直流 → 4点移动平均滤波 → 一阶差分 → 汉明窗
 *   - 峰值检测得到心跳波峰位置，计算平均波峰间隔 → 心率
 *   - 在波谷间段内计算红光/红外的 AC/DC 比值 → 查表得 SpO2
 *
 * @param pun_ir_buffer      红外（IR）原始采样数组，长度 n_ir_buffer_length
 * @param n_ir_buffer_length 缓冲区长度，应等于 BUFFER_SIZE（500）
 * @param pun_red_buffer     红光（RED）原始采样数组，长度同上
 * @param pn_spo2            输出：SpO2 值（%），无效时输出 -999
 * @param pch_spo2_valid     输出：SpO2 是否有效（1=有效，0=无效）
 * @param pn_heart_rate      输出：心率（bpm），无效时输出 -999
 * @param pch_hr_valid       输出：心率是否有效（1=有效，0=无效）
 * @param fs 连续原始样本的采样频率 Hz；0 无效。输出直接为 bpm，禁止二次缩放。
 */
void maxim_heart_rate_and_oxygen_saturation(uint32_t *pun_ir_buffer,
                                             int32_t   n_ir_buffer_length,
                                             uint32_t *pun_red_buffer,
                                             int32_t  *pn_spo2,
                                             int8_t   *pch_spo2_valid,
                                             int32_t  *pn_heart_rate,
                                             int8_t   *pch_hr_valid,
                                             uint32_t  fs);

/* 旧接口保留，返回 0 表示不可用，不是血流速度测量。 */
float maxim_blood_flow_rate(uint32_t *pun_ir_buffer, int32_t n_ir_buffer_length);

/* ======================== 内部辅助函数（外部一般不直接调用） ======================== */

/**
 * @brief 在数组中寻找满足条件的波峰
 *
 * 综合调用以下两个函数：先找高于阈值的峰，再删除距离过近的峰，最后限制数量。
 *
 * @param pn_locs        输出：波峰位置索引数组
 * @param pn_npks        输出：找到的波峰数量
 * @param pn_x           输入：信号数组
 * @param n_size         信号长度
 * @param n_min_height   最小波峰高度阈值
 * @param n_min_distance 波峰之间的最小间隔（采样点数）
 * @param n_max_num      最多保留的波峰数量
 */
void maxim_find_peaks(int32_t *pn_locs, int32_t *pn_npks,
                      int32_t *pn_x, int32_t n_size,
                      int32_t n_min_height, int32_t n_min_distance, int32_t n_max_num);

void maxim_peaks_above_min_height(int32_t *pn_locs, int32_t *pn_npks,
                                   int32_t *pn_x, int32_t n_size, int32_t n_min_height);

void maxim_remove_close_peaks(int32_t *pn_locs, int32_t *pn_npks,
                               int32_t *pn_x, int32_t n_min_distance);

void maxim_sort_ascend(int32_t *pn_x, int32_t n_size);

void maxim_sort_indices_descend(int32_t *pn_x, int32_t *pn_indx, int32_t n_size);

#endif /* __PPG_PROCESS_H__ */
