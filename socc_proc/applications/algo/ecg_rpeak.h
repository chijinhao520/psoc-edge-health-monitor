/*
 * ecg_rpeak.h — M33 侧 ECG R 峰计时（PAT 用，逐字复用 M55 R 峰检测器）
 *
 * 本文件与 Edgi_Talk_M55_LVGL/applications/arrhythmia_preproc.h 核心算法逐字
 * 一致（同一份 Pan-Tompkins 风格检测器，已在 MIT-BIH 上验证 R 峰 Se/PPV
 * 99.9-100%），只搬运不改动任何阈值/系数/窗长。
 *
 * 这是一份独立拷贝，跟 M55 版用途不同：
 *   - M55 版（arrhythmia_preproc.c/h）：给心律不齐 NPU 分类模型准备输入
 *     （256 窗 + R-R 特征），必须跟训练管线逐字一致，不能随便调。
 *   - M33 版（本文件）：只用来算 R 峰的绝对时间戳，喂给 PAT（脉搏波传导
 *     时间）配对逻辑，不做分类、不需要 256 窗输出。
 * 两份文件目前保持算法完全一致（方便交叉验证两边测出的心率/R-R 是否吻合），
 * 不代表以后必须同步每一次改动——如果 M55 侧未来为分类效果调整阈值，
 * M33 侧的计时精度不受影响，可以独立决定是否跟进。
 *
 * 把 M33 侧现有 ADS1015 500Hz(实测 ~388Hz) ECG 流转成 R 峰事件流：
 *   500Hz 线性重采样→360Hz → butter 5-15Hz 带通(sos) → 5 点导数 → 平方
 *   → 150ms 因果滑窗积分 → 自适应双阈值+200ms 不应期+searchback
 *   → R 精定位(原始信号窗内幅值极大)
 *
 * 纯 C(不依赖 RT-Thread)，与 M55 版一样可在 PC 单测。
 */

#ifndef __ECG_RPEAK_H__
#define __ECG_RPEAK_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARR_PP_FS_IN    500     /* Standalone default; live IPC requires measured rate. */
#define ARR_PP_FS       360     /* 内部处理采样率(与 M55 版一致,不可改) */
#define ARR_PP_WIN_LEN  256     /* 心拍窗长(本文件不使用,保留字段对齐 M55 版结构) */
#define ARR_PP_RR_DIM   3

/** @brief 复位前处理状态(开始采集前调一次)。*/
void arr_preproc_reset(void);
/* Changing the input rate resets all timing/filter history. */
int arr_preproc_set_sample_rate(float hz);

/**
 * @brief 喂一个 AD8232 原始采样(500Hz),驱动整条流水线。
 *
 * @param sample_500  一个 ECG 采样(任意线性单位,内部自归一)
 * @param ecg_out     [256] 准备好的心拍窗(min-max 到 [0,1]),仅返回 1 时有效
 * @param rr_out      [3]   R-R 特征,仅返回 1 时有效
 * @return 1 = 产出一个心拍(ecg_out/rr_out 已填),0 = 暂无
 *
 * 注意（PAT 用途）：R 峰的"检出时刻"比这个函数真正返回 1 的时刻要早得多
 * （返回延迟约 1 拍/0.4-0.8s，因为要等 post-RR 特征）。要拿 R 峰的近似
 * 绝对时间戳做 PAT 配对，请用 arr_preproc_rpeak_count() 的增量判定
 * ——每次调用本函数后检查这个计数是否比上次多了 1，多了就在那一刻记
 * rt_tick_get() 作为该 R 峰的时间戳（含算法固有的检测延迟，需要标定修正，
 * 见 pat_calc.c）。
 */
int arr_preproc_push_500(float sample_500,
                         float ecg_out[ARR_PP_WIN_LEN],
                         float rr_out[ARR_PP_RR_DIM]);

/** @brief 已检出 R 峰总数(诊断用，PAT 配对靠这个的增量判定新 R 峰)。*/
uint32_t arr_preproc_rpeak_count(void);

/**
 * @brief 最近一次登记的 R-R 间期，单位 ms(供 HRV/呼吸率用，跟分类特征
 * rr_out 的归一化比值不同，这个是真实时间间隔)。
 *
 * @return 最近一次 R-R 间期(ms)；一次都没检出过 R 峰时返回 0
 */
float arr_preproc_last_rr_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* __ECG_RPEAK_H__ */
