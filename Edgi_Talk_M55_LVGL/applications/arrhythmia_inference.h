/*
 * arrhythmia_inference.h — TFLite Micro + Ethos-U55 NPU 心律不齐推理接口
 *
 * 提供给 arrhythmia_task.c / arrhythmia_test.cpp 调用的 C ABI 封装,
 * 内部实现在 arrhythmia_inference.cpp。移植自 glucose_inference 同套 5 步模式
 * (heap 分配 + placement new + AddEthosU),见 memory 2026-socc-tflite-port-pattern。
 *
 * 模型输入(两路,与训练 data_mitbih.py 强契约):
 *   ecg : [1,256,1] int8 — 单导联 ECG 心拍,R 峰对齐 256 样本,每拍 min-max→[0,1]
 *   rr  : [1,3]     int8 — R-R 间期特征 [pre/med, post/med, pre/local],各 clip[0,2]/2→[0,1]
 * 模型输出:
 *   [1,4] int8 → 反量化为 4 类 softmax 概率,argmax 得类别
 *
 * 类别 (AAMI,已 drop 退化的 Q):
 *   0 = N 正常 | 1 = S 房性 | 2 = V 室早 | 3 = F 融合
 */

#ifndef __ARRHYTHMIA_INFERENCE_H__
#define __ARRHYTHMIA_INFERENCE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARR_ECG_LEN   256
#define ARR_RR_DIM    3
#define ARR_N_CLASS   4

/**
 * @brief 初始化 TFLite Micro 解释器并分配 tensor(一次性)
 * @return 0 成功,负数失败(见 cpp 内部错误码 -1..-20)
 *
 * 失败后 is_ready 返回 0,上层走 L3 fallback(规则判据)。
 */
int arrhythmia_inference_init(void);

/**
 * @brief 查询推理引擎是否就绪
 * @return 1 = 可调 invoke,0 = init 未成功 / NPU 不可用
 */
int arrhythmia_inference_is_ready(void);

/**
 * @brief 单拍推理
 *
 * @param ecg       ECG 心拍 [256],已 R 峰对齐 + min-max 归一到 [0,1]
 * @param rr        R-R 特征 [3] = [pre/med, post/med, pre/local],各 [0,1]
 * @param out_class 输出类别 0..3 (N/S/V/F)
 * @param out_conf  输出该类 softmax 置信度 [0,1](可传 NULL 不取)
 * @param out_probs 输出 4 类概率 [4](可传 NULL 不取)
 *
 * @return 0 成功,负数失败:
 *           -1: 引擎未就绪
 *           -2: 量化参数非法
 *           -3: Invoke 返回错误
 */
int arrhythmia_inference_invoke(const float ecg[ARR_ECG_LEN],
                                const float rr[ARR_RR_DIM],
                                int *out_class,
                                float *out_conf,
                                float out_probs[ARR_N_CLASS]);

/**
 * @brief 获取 tensor_arena 实际使用字节数(init 后有效),供调优。
 */
uint32_t arrhythmia_inference_arena_used(void);

/** @brief 类别短名 "N"/"S"/"V"/"F"(out_class 越界返回 "?")。*/
const char *arrhythmia_class_name(int cls);

#ifdef __cplusplus
}
#endif

#endif /* __ARRHYTHMIA_INFERENCE_H__ */
