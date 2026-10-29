/*
 * PPG 原始波形下采样模块
 *
 * 将 500 点 (5s @ 100Hz) 原始 ADC 数据下采样为 128 点 × 2 通道（RED+IR），
 * 作为血糖 CNN 模型的输入特征。
 *
 * 算法：4 点平均池化 500→125 → 尾部复制填充到 128 → 通道内 min-max 归一化 [0,1]
 */

#ifndef __PPG_DOWNSAMPLE_H__
#define __PPG_DOWNSAMPLE_H__

#include <stdint.h>

#define PPG_DS_LEN      128   /* 下采样后每通道点数 */
#define PPG_DS_CHANNELS 2     /* 通道数: [0]=RED, [1]=IR */
#define PPG_DS_POOL     4     /* 池化窗口大小 */

/**
 * @brief PPG 原始波形 4 倍平均池化下采样
 *
 * 每 4 个连续采样取均值 → 125 点，尾部复制最后值填充到 128 点，
 * 通道内 min-max 归一化到 [0, 1]。
 *
 * @param red   RED 通道原始 ADC 数据，长度 n（18-bit uint32）
 * @param ir    IR 通道原始 ADC 数据，长度 n
 * @param n     原始数据点数（应为 500）
 * @param out   输出 [PPG_DS_LEN][PPG_DS_CHANNELS]，out[t][0]=RED, out[t][1]=IR
 */
void ppg_downsample(const uint32_t *red, const uint32_t *ir, int n,
                    float out[PPG_DS_LEN][PPG_DS_CHANNELS]);

#endif /* __PPG_DOWNSAMPLE_H__ */
