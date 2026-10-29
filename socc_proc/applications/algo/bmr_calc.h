/*
 * BMR（基础代谢率）计算模块
 *
 * 基于皮肤热辐射 + 自然对流 + 蒸发散热三项物理模型估算 BMR，
 * 结果作为血糖估算模型的输入特征之一。
 *
 * 纯数学模块，与 RT-Thread / 硬件平台无关。
 *
 * 公式来源：SocChina 2025 博流赛道参考项目 main.c:994-1023
 *
 *   BMR = (辐射项 + 对流项 + 蒸发项) / 3.6
 *
 *   辐射项 = 0.99 × 5.89e-8 × (T_ir + 273.15)^4
 *   对流项 = 2.38 × (T_finger - T_env)^1.25      [需 T_finger > T_env]
 *   蒸发项 = 3.054 × (0.256 × T_finger - 3.37) × (1 - H_env / 100)
 *
 * 输入说明：
 *   t_ir     — 红外测温（MLX90614 物体温度），degC
 *   t_finger — 手指接触温度（SHT30 温度），degC
 *   t_env    — 环境温度（MLX90614 环境温度），degC
 *   h_env    — 环境相对湿度（SHT30 湿度），%RH
 */

#ifndef __BMR_CALC_H__
#define __BMR_CALC_H__

/* ------------------------------------------------------------------ */
/*  生理合理性边界（超出范围视为传感器异常）                            */
/* ------------------------------------------------------------------ */

#define BMR_T_IR_MIN      28.0f   /* 红外体温下限，degC */
#define BMR_T_IR_MAX      43.0f   /* 红外体温上限，degC */
#define BMR_T_FINGER_MIN  20.0f   /* 手指温度下限，degC */
#define BMR_T_FINGER_MAX  42.0f   /* 手指温度上限，degC */
#define BMR_T_ENV_MIN      0.0f   /* 环境温度下限，degC */
#define BMR_T_ENV_MAX     50.0f   /* 环境温度上限，degC */
#define BMR_H_ENV_MIN      0.0f   /* 湿度下限，%RH */
#define BMR_H_ENV_MAX    100.0f   /* 湿度上限，%RH */

/* ------------------------------------------------------------------ */
/*  API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief 校验输入是否在生理合理范围内
 *
 * @return 1 = 合法，0 = 超出范围（传感器可能异常）
 */
int bmr_input_valid(float t_ir, float t_finger, float t_env, float h_env);

/**
 * @brief 计算 BMR
 *
 * 若输入超出合理范围，返回 0.0f 并不执行计算。
 * 若 t_finger <= t_env（皮肤不高于环境），对流项置零（避免对负数取小数次幂）。
 *
 * @return BMR 估算值（kcal/h），失败返回 0.0f
 */
float bmr_calc(float t_ir, float t_finger, float t_env, float h_env);

#endif /* __BMR_CALC_H__ */
