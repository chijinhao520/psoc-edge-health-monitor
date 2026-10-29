/*
 * glucose_inference.h — TFLite Micro + Ethos-U55 NPU 血糖推理接口
 *
 * 提供给 glucose_task.c 调用的 C ABI 封装,内部实现在 glucose_inference.cpp。
 *
 * 三段接口:
 *   - glucose_inference_init()      一次性初始化(加载模型 + 分配 tensor + 缓存指针)
 *   - glucose_inference_is_ready()  查询是否就绪(供 L1/L3 fallback 判断)
 *   - glucose_inference_invoke()    单帧推理(float→int8 量化 + Invoke + int8→float 反量化)
 *
 * 模型输入(两路):
 *   ppg  : [1,128,1,2] int8  — PPG 波形 RED/IR 通道,已归一化到 [0,1]
 *   scal : [1,7]       int8  — 7 维标量特征,顺序: HR/SpO2/BV/BMR/t_ir/t_env/h_env
 * 模型输出:
 *   [1,1] int8 → 反量化到 mmol/L (clamp 到 [2.0, 20.0])
 */

#ifndef __GLUCOSE_INFERENCE_H__
#define __GLUCOSE_INFERENCE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 TFLite Micro 解释器并分配 tensor
 *
 * 内部步骤:
 *   1. 检查 npu_is_ready() (来自 npu_init.c)
 *   2. tflite::GetModel(g_glucose_model)
 *   3. MicroMutableOpResolver<1>::AddEthosU()
 *   4. MicroInterpreter::AllocateTensors()
 *   5. 缓存 input(0)/input(1)/output(0) 指针
 *
 * @return 0 成功,负数失败(见 cpp 内部错误码)
 *
 * 推荐在 glucose_thread_entry 启动延迟内调用一次(NPU 就绪后)。
 * 失败后 is_ready 返回 0,task 自动降级到 L3 ridge fallback。
 */
int glucose_inference_init(void);

/**
 * @brief 查询推理引擎是否可用
 * @return 1 = 可调用 invoke,0 = init 未成功或 NPU 不可用
 */
int glucose_inference_is_ready(void);

/**
 * @brief 单帧推理
 *
 * @param ppg  PPG 波形 [128][2],已归一化到 [0,1]
 *             ppg[i][0] = RED 通道,ppg[i][1] = IR 通道
 * @param scal 7 维标量特征,顺序:
 *             scal[0] = HR (bpm,例 75)
 *             scal[1] = SpO2 (%,例 98)
 *             scal[2] = BV (相对血流速度,例 1.0)
 *             scal[3] = BMR (W,例 40)
 *             scal[4] = t_ir (°C,MLX90614 TOBJ1,例 36.5)
 *             scal[5] = t_env (°C,SHT30 环境温度,例 25)
 *             scal[6] = h_env (%RH,SHT30 湿度,例 55)
 * @param out_mmol 输出血糖估算值 (mmol/L),clamp 到 [2.0, 20.0]
 *
 * @return 0 成功,负数失败:
 *           -1: 引擎未就绪
 *           -2: 量化参数非法 (scale = 0)
 *           -3: Invoke 返回错误
 *           -4: 输出反量化越界
 */
int glucose_inference_invoke(const float ppg[128][2],
                             const float scal[7],
                             float *out_mmol);

/**
 * @brief 获取 tensor_arena 实际使用字节数(只在 init 后有效)
 *        供 glucose_test 命令打印调优用。
 */
uint32_t glucose_inference_arena_used(void);

#ifdef __cplusplus
}
#endif

#endif /* __GLUCOSE_INFERENCE_H__ */
