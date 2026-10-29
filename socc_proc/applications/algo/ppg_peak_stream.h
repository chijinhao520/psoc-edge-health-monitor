/*
 * PPG 主通道实时峰检测（PAT 用）
 *
 * 检测的是原始 IR 信号"最陡上升沿"（一阶差分局部极大），不是波形几何峰值
 * ——这跟本项目 ppg_process.c 里批量心率算法的既有惯例一致（它也是在差分
 * 信号里找峰，对应原始信号的最陡上升沿，见 ppg_process.c Step3 注释），
 * 同时"最陡上升沿/foot-to-foot"也是 PAT/PTT 文献里常用的特征点，比几何
 * 峰值更不容易受波形反射畸变影响。
 *
 * 纯数学算法，不依赖 RTOS——时间戳不在本文件内产生，由调用方（ppg_task）
 * 在检出峰后自行调用 rt_tick_get() 记录，本文件只管"是不是峰"。
 *
 * 使用流程：
 *   1. ppg_peak_state_reset() 复位状态（常驻状态，不要每 5s 批次重置，
 *      否则自适应阈值包络和不应期计数会在批次边界丢失连续性）
 *   2. 每个 IR 原始采样调用 ppg_peak_process_sample()
 */

#ifndef __PPG_PEAK_STREAM_H__
#define __PPG_PEAK_STREAM_H__

#include <stdint.h>

/* 100Hz 下 300ms 不应期，对应生理心率上限 ~200bpm（现场标定用，非严格推导值） */
#define PPG_PEAK_REFRACTORY_SAMPLES  30
#define PPG_PEAK_DIFF_EMA_ALPHA      0.3f   /* 一阶差分平滑系数，抑制采样噪声假峰 */
#define PPG_PEAK_THR_EMA_ALPHA       0.05f  /* 自适应阈值包络平滑系数（慢速跟踪） */

typedef struct
{
    int32_t prev_raw;
    int     have_prev_raw;

    float   diff_ema;
    float   diff_prev;
    float   diff_pprev;
    int     have_diff_hist;

    float   thr_ema;              /* 自适应阈值：近期差分峰值包络的一半 */
    int     samples_since_peak;
    int     initialized;
} ppg_peak_state_t;

/** @brief 复位状态（开始采集前调一次，之后常驻，不要按批次重置） */
void ppg_peak_state_reset(ppg_peak_state_t *st);

/**
 * @brief 喂一个原始 IR 采样，检测是否为"最陡上升沿"特征点
 *
 * @param st      状态句柄
 * @param raw_ir  原始 IR 采样（MAX30102 FIFO 读数）
 * @return 1 = 检出一个峰（约滞后 1 个采样点，100Hz 下约 10ms，可忽略），0 = 不是
 */
int ppg_peak_process_sample(ppg_peak_state_t *st, uint32_t raw_ir);

#endif /* __PPG_PEAK_STREAM_H__ */
