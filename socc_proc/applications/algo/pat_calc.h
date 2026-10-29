/*
 * pat_calc.h — PAT（Pulse Arrival Time，脉搏波传导时间）配对计算
 *
 * ecg_task 检出新 R 峰、ppg_task 检出新 PPG 峰时分别调用 pat_on_r_peak() /
 * pat_on_ppg_peak()，内部维护"最近一个未配对的 R 峰"并做时间窗配对。
 *
 * 跟本项目 algo 目录下其它源码不同：本文件需要跨线程共享状态（ecg_task 和
 * ppg_task 是两个独立线程），所以内部用 RT-Thread 互斥锁保护，不是纯
 * 平台无关数学代码——这是有意的例外，PAT 本身就是跨线程事件相关性计算。
 */

#ifndef __PAT_CALC_H__
#define __PAT_CALC_H__

#include <rtthread.h>

/*
 * 生理配对窗口（原始时间差，修正检测延迟前）：R 峰之后落在这个范围内的
 * PPG 峰才认为是同一拍，窗口外一律丢弃、不强配（避免把这一拍误配到下一
 * 拍）。上限故意留得比生理 PAT 宽，因为 R 峰记录时刻本身滞后于真实 R 峰
 * （算法固有检测延迟，见下）。
 */
#define PAT_RAW_MIN_MS   50
#define PAT_RAW_MAX_MS   700

/*
 * R 峰检测算法固有延迟修正常数（ms）——ecg_rpeak.c 的因果滑窗积分(MWI)
 * + searchback 回溯搜索本身带来的滞后，量级 ~150-380ms。这是工程经验
 * 初值，不是严格推导值，需要现场用同步参考信号标定后更新。
 */
#define PAT_DETECT_LATENCY_MS  250.0f

/* 修正后 PAT 的合理性范围：超出视为配对失败/噪声，标 invalid 而非硬写数值 */
#define PAT_VALID_MIN_MS  0.0f
#define PAT_VALID_MAX_MS  600.0f

/** @brief 初始化内部状态（建互斥锁），由 INIT_ENV_EXPORT 自动调用 */
int pat_calc_init(void);

/** @brief ecg_task 检出新 R 峰时调用，记为"待配对" */
void pat_on_r_peak(rt_tick_t tick);

/**
 * @brief ppg_task 检出新 PPG 峰时调用
 *
 * 若存在待配对 R 峰且落在生理窗口内，计算 PAT 并写入 sensor_data（继而
 * 由 bmr_task 5s consolidation 写入 IPC）；不满足条件则丢弃这次 PPG 峰，
 * 不强行配对。
 */
void pat_on_ppg_peak(rt_tick_t tick);

#endif /* __PAT_CALC_H__ */
