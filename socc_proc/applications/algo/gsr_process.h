/*
 * GSR (Grove 皮肤电导) 实时处理算法头文件
 *
 * 纯数学算法，与硬件平台无关。
 *
 * 无校准硬件（无标准电导参考），本模块不换算绝对电导单位，只输出相对
 * 个人基线的 0-100 应激指数——风格同 ppg_process.h 的 PPG-SQI：不追求
 * 绝对精度，追求"方向对、有区分度"。
 *
 * 使用流程：
 *   1. gsr_state_reset() 复位状态
 *   2. 每次采样调用 gsr_process_sample()，喂原始 ADS1015 读数（mV）
 *   3. 学习期（GSR_LEARN_SAMPLES 个采样）结束前返回 0（未就绪），
 *      之后每次返回 1 并输出应激指数
 */

#ifndef __GSR_PROCESS_H__
#define __GSR_PROCESS_H__

#include <stdint.h>

/* ======================== 参数（现场标定用，非严格科研阈值） ======================== */

#define GSR_EMA_ALPHA        0.1f   /* EMA 平滑系数，与 ADR 里"EMA(α=0.1)+5s 滑窗"一致 */
#define GSR_LEARN_SAMPLES    250    /* 学习期采样数：50Hz 下约 5s，先建个人基线 */
#define GSR_WIN_LEN          50     /* 短期变异滑窗长度：50Hz 下约 1s */

/* ======================== 状态结构体 ======================== */

typedef struct
{
    float    ema_mv;              /* EMA 平滑后的读数，mV */
    float    baseline_mv;         /* 学习期结束时锁定的个人基线，mV */
    int      learned;             /* 1 = 基线已建立 */
    uint32_t sample_count;        /* 已处理采样数 */
    float    win_buf[GSR_WIN_LEN];/* 短期变异滑窗（存 EMA 值） */
    int      win_idx;
    int      win_filled;
} gsr_state_t;

/** @brief 复位状态（开始采集前调一次） */
void gsr_state_reset(gsr_state_t *st);

/**
 * @brief 喂一个原始采样，驱动 EMA 平滑 + 应激指数计算
 *
 * @param st                状态句柄
 * @param raw_mv            原始 ADC 读数，mV（ADS1015 ±2.048V 量程下 1 code ~= 1mV）
 * @param stress_index_out  输出：应激指数 0-100（学习期内固定输出 0）
 * @return 1 = 已过学习期；0 = 学习中或输入无效。此分数未经应激参考标定。
 * 输入零码、负值、饱和或非有限值会清基线；调用者在通信失败/断流时也须复位。
 */
int gsr_process_sample(gsr_state_t *st, float raw_mv, float *stress_index_out);

#endif /* __GSR_PROCESS_H__ */
