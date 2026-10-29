/*
 * BMR（基础代谢率）计算实现
 */

#include <math.h>
#include "bmr_calc.h"

/* Stefan-Boltzmann 常数（W·m⁻²·K⁻⁴） */
#define SIGMA  5.89e-8f

int bmr_input_valid(float t_ir, float t_finger, float t_env, float h_env)
{
    if (t_ir     < BMR_T_IR_MIN     || t_ir     > BMR_T_IR_MAX)     return 0;
    if (t_finger < BMR_T_FINGER_MIN || t_finger > BMR_T_FINGER_MAX) return 0;
    if (t_env    < BMR_T_ENV_MIN    || t_env    > BMR_T_ENV_MAX)     return 0;
    if (h_env    < BMR_H_ENV_MIN    || h_env    > BMR_H_ENV_MAX)     return 0;
    return 1;
}

float bmr_calc(float t_ir, float t_finger, float t_env, float h_env)
{
    float radiation, convection, evaporation, delta_t;

    if (!bmr_input_valid(t_ir, t_finger, t_env, h_env))
        return 0.0f;

    /* 辐射项：皮肤红外辐射功率（Stefan-Boltzmann） */
    radiation = 0.99f * SIGMA * powf(t_ir + 273.15f, 4.0f);

    /* 对流项：自然对流散热，等效于 2.38 × ΔT^1.25
     * 当皮肤温度不高于环境时对流项为零（室温倒灌场景，通常不发生） */
    delta_t = t_finger - t_env;
    if (delta_t > 0.0f)
        convection = 2.38f * powf(delta_t, 0.25f) * delta_t;
    else
        convection = 0.0f;

    /* 蒸发项：皮肤水分蒸发散热，受环境湿度抑制 */
    evaporation = 3.054f * (0.256f * t_finger - 3.37f) * (1.0f - h_env * 0.01f);

    /* 换算为 kcal/h */
    return (radiation + convection + evaporation) / 3.6f;
}
