/*
 * glucose_task.h — M55 血糖推理任务接口
 *
 * glucose_task 在 M55 上运行，从 IPC 共享内存读取：
 *   - PPG 下采样波形 (128×2 float)  ← ppg_waveform_ipc_t @ 0x261C0C00
 *   - 标量特征 (HR, SpO2, BV, BMR, t_ir, sht30_temp/humi) ← sensor_ipc_data_t @ 0x261C0800
 *
 * 推理结果存于 M55 本地 static 变量（不写回 IPC，避免 bmr_task 覆盖竞争）。
 * ui_data.c 通过 glucose_get_result() 直接读取（同核，无需 IPC）。
 */

#ifndef __GLUCOSE_TASK_H__
#define __GLUCOSE_TASK_H__

#include <stdint.h>

#define GLU_SRC_NPU          0   /* TFLite Micro + Ethos-U55 真实模型推理 */
#define GLU_SRC_PLACEHOLDER  1   /* 占位伪公式（无医学意义，仅通路验证） */

typedef struct {
    float    glucose;      /* 血糖估算值，mmol/L */
    int      valid;        /* 1 = 真实模型推理结果。占位公式产物一律 0 ——
                            * "宁可显示 -- 也不能编数"：valid 表达的是
                            * "这个数能不能给人看"，不是"推理跑没跑过" */
    int      source;       /* GLU_SRC_NPU / GLU_SRC_PLACEHOLDER，随每轮更新 */
    uint32_t tick;         /* 推理完成时间戳 (rt_tick_get) */
    uint32_t wf_seq;       /* 对应的波形 IPC sequence */
    uint32_t measurement_id;
} glucose_result_t;

/**
 * @brief 获取最新推理结果
 *
 * 返回指向 static 结构体的指针，始终有效。
 * valid==0 表示尚无可展示的结果（未推理过，或仅有占位公式产物 ——
 * 后者的数值仍写入 glucose 字段供调试观察，但 UI/评分/建议不得使用）。
 */
const glucose_result_t *glucose_get_result(void);
void glucose_copy_result(glucose_result_t *out);

#endif /* __GLUCOSE_TASK_H__ */
