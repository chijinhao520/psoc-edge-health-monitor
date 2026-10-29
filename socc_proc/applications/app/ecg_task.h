/*
 * ecg_task.h — ECG 采集任务对外接口
 *
 * 除了常规的 ecg_task_init()，额外暴露 R 峰计时结果，供 PAT（脉搏波传导
 * 时间）配对逻辑（pat_calc.c）消费——R 峰检测本身由 algo/ecg_rpeak.c 在
 * 本任务的采样循环里同步驱动，详见 ecg_task.c。
 */

#ifndef __ECG_TASK_H__
#define __ECG_TASK_H__

#include <rtthread.h>
#include <stdint.h>

int ecg_task_init(void);

/**
 * @brief 最近一次检出 R 峰的近似绝对时间戳
 *
 * 记录时机是 arr_preproc_rpeak_count() 计数增量的那一刻，含算法固有的
 * 检测延迟（refine_r 回溯搜索窗口，量级 ~100-200ms）——这是一个需要标定
 * 的常数偏移，不是抖动，PAT 配对时需要减去这个标定值。
 */
rt_tick_t ecg_task_last_rpeak_tick(void);

/**
 * @brief R 峰事件序号，单调递增
 *
 * 每检出一个新 R 峰加 1。消费者（PAT 配对）靠这个判断"是否是没处理过的
 * 新 R 峰"，避免重复配对同一个 R 峰。
 */
uint32_t ecg_task_rpeak_seq(void);

#endif /* __ECG_TASK_H__ */
