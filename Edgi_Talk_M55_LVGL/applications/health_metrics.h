/*
 * health_metrics.h — HRV(RMSSD/SDNN) + 呼吸率 DSP 派生指标
 *
 * 纯数值计算，不训练模型，喂 R-R 间期(ms)即可，跟 arrhythmia 的 R 峰检测
 * 共用同一条 ECG 前处理管线（每检出一拍调一次 health_metrics_feed_rr）。
 *
 * 呼吸率算法说明：Karlen 2013 原版融合"基线漂移+幅度调制+频率调制"三个
 * 呼吸诱导特征。这里只实现**频率调制(RSA，呼吸性窦性心律不齐)**这一个
 * 特征——R-R 间期序列本身随呼吸周期起伏，短均值去噪+长均值去趋势后数
 * 过零次数估计呼吸周期。省了幅度/基线两路(那两路需要原始 ECG 幅度信息，
 * 这里只有 R-R 间期序列)，精度弱于完整版，标"实验性"展示。
 */

#ifndef __HEALTH_METRICS_H__
#define __HEALTH_METRICS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    float    hrv_rmssd_ms;   /* 相邻 R-R 差值均方根，短时心率变异性 */
    float    hrv_sdnn_ms;    /* R-R 间期标准差，整体心率变异性 */
    float    resp_rate_bpm;  /* 呼吸率估计，次/分钟(RSA 简化法，实验性) */
    int32_t  hrv_valid;      /* 连续窗口 >=30 秒且样本数够才认为可信 */
    int32_t  resp_valid;     /* 连续窗口 >=30 秒且样本数够才认为可信 */
    uint32_t rr_count;       /* 累计喂入的 R-R 间期总数(诊断用) */
} health_metrics_result_t;

/** @brief 复位所有状态(开始采集前调一次)。*/
void health_metrics_reset(void);

/**
 * @brief 喂一个新检出的 R-R 间期，驱动 HRV/呼吸率重新计算。
 *
 * @param rr_ms R-R 间期，单位 ms；生理范围外(<300 或 >2000ms)会被丢弃
 */
void health_metrics_feed_rr(float rr_ms);

/* 线程间读取使用副本接口；原指针接口仅保留兼容。 */
void health_metrics_copy_result(health_metrics_result_t *out);

/** @brief 获取当前结果(只读，指向内部静态结构)。*/
const health_metrics_result_t *health_metrics_get_result(void);

#ifdef __cplusplus
}
#endif

#endif /* __HEALTH_METRICS_H__ */
