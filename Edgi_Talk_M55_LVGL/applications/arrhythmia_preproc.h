/*
 * arrhythmia_preproc.h — AD8232 ECG 实时前处理(部署契约)
 *
 * 把 AD8232 的 500Hz ECG 流转成模型输入(256 窗 + 3 维 R-R 特征),
 * 与训练 data_mitbih.py / 验证 deploy_sim.py **逐字一致**(已在 MIT-BIH 上
 * 验证: R 峰 Se/PPV 99.9-100%, 端到端 V F1 ~0.97 / N ~0.99)。
 *
 * 流水线(全部在重采样后的 360Hz 域,与训练同采样率):
 *   500Hz 线性重采样→360Hz → butter 5-15Hz 带通(sos) → 5 点导数 → 平方
 *   → 150ms 因果滑窗积分 → 自适应双阈值+200ms 不应期+searchback
 *   → R 精定位(原始信号窗内幅值极大) → 256 窗(R±128) min-max
 *   → R-R 特征 [pre/med, post/med, pre/local]
 *
 * 纯 C(不依赖 RT-Thread),可在 PC 单测。每拍因需 post-RR + 未来半窗,
 * 延迟约 1 拍(~0.4-0.8s)后产出,适合监护场景。
 */

#ifndef __ARRHYTHMIA_PREPROC_H__
#define __ARRHYTHMIA_PREPROC_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARR_PP_FS_IN    500     /* Standalone default; live IPC requires measured rate. */
#define ARR_PP_FS       360     /* 模型采样率(与 MIT-BIH 训练一致,不可改) */
#define ARR_PP_WIN_LEN  256     /* 心拍窗长 */
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
 */
int arr_preproc_push_500(float sample_500,
                         float ecg_out[ARR_PP_WIN_LEN],
                         float rr_out[ARR_PP_RR_DIM]);

/** @brief 已检出 R 峰总数(诊断用)。*/
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

#endif /* __ARRHYTHMIA_PREPROC_H__ */
