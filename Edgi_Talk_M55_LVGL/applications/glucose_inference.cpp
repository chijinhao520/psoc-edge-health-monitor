/*
 * glucose_inference.cpp — Stage S3: 真正跑 NPU 推理
 *
 * 相比 S2 增加:
 *   - is_ready() 改返回 s_s2_init_done (S2 init 完成后变 1)
 *   - invoke() 完整实现: 量化 → Invoke → 反量化
 *
 * 路径全图:
 *   1. glucose_task 启动延迟 15s
 *   2. 调 glucose_inference_init() → S2 完整 TFLite 构造 + AllocateTensors
 *   3. init 成功 → is_ready=1 → task 后续走 NPU 路径
 *   4. 每 5s 周期: 量化 PPG+SCAL → Invoke (NPU ~20μs) → 反量化 → mmol/L
 *
 * 全部 buffer 在 heap (m55_data_secondary 1.4MB),DTCM 不动。
 */

#include <rtthread.h>
#include <stdint.h>
#include <math.h>
#include <new>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "glucose_model.h"

extern "C" {
#include "glucose_inference.h"
extern int npu_is_ready(void);
}

#define GLUCOSE_TENSOR_ARENA_SIZE  (32 * 1024)
#define GLUCOSE_OPS_RESOLVER_SIZE  2
#define PPG_LEN                    128
#define PPG_CHANNELS               2
#define SCAL_DIM                   7
#define GLUCOSE_MIN_MMOL           2.0f
#define GLUCOSE_MAX_MMOL           20.0f

namespace {

uint8_t *s_tensor_arena         = nullptr;
uint8_t *s_resolver_storage     = nullptr;
uint8_t *s_interpreter_storage  = nullptr;

tflite::MicroMutableOpResolver<GLUCOSE_OPS_RESOLVER_SIZE> *s_resolver    = nullptr;
tflite::MicroInterpreter                                   *s_interpreter = nullptr;

const tflite::Model *s_model     = nullptr;
TfLiteTensor        *s_input_ppg  = nullptr;
TfLiteTensor        *s_input_scal = nullptr;
TfLiteTensor        *s_output     = nullptr;

float    s_ppg_scale  = 0.0f;  int32_t s_ppg_zp  = 0;
float    s_scal_scale = 0.0f;  int32_t s_scal_zp = 0;
float    s_out_scale  = 0.0f;  int32_t s_out_zp  = 0;

uint32_t s_arena_used   = 0;
int      s_s2_init_done = 0;

__attribute__((used))
const uint8_t *const s_model_anchor = g_glucose_model;

} // anonymous namespace

/* ------------------------------------------------------------------ */
/*  量化辅助                                                            */
/* ------------------------------------------------------------------ */

static inline int8_t quantize_to_int8(float f, float scale, int32_t zero_point)
{
    int32_t q = (int32_t)lroundf(f / scale) + zero_point;
    if (q < -128) q = -128;
    if (q >  127) q =  127;
    return (int8_t)q;
}

static inline float dequantize_from_int8(int8_t q, float scale, int32_t zero_point)
{
    return ((float)q - (float)zero_point) * scale;
}

/* ------------------------------------------------------------------ */
/*  API: init (与 S2 相同)                                              */
/* ------------------------------------------------------------------ */

extern "C" int glucose_inference_init(void)
{
    if (s_s2_init_done)
    {
        return 0;
    }

    rt_kprintf("[glucose_inference S3] init enter\n");

    if (!npu_is_ready())
    {
        rt_kprintf("[glucose_inference S3] NPU not ready\n");
        return -1;
    }

    if (s_tensor_arena == RT_NULL)
    {
        s_tensor_arena = (uint8_t *)rt_malloc_align(GLUCOSE_TENSOR_ARENA_SIZE, 16);
        if (s_tensor_arena == RT_NULL) return -10;
    }

    if (s_resolver == nullptr)
    {
        size_t sz    = sizeof(tflite::MicroMutableOpResolver<GLUCOSE_OPS_RESOLVER_SIZE>);
        size_t align = alignof(tflite::MicroMutableOpResolver<GLUCOSE_OPS_RESOLVER_SIZE>);

        s_resolver_storage = (uint8_t *)rt_malloc_align(sz, align);
        if (s_resolver_storage == RT_NULL) return -11;

        s_resolver = new (s_resolver_storage)
            tflite::MicroMutableOpResolver<GLUCOSE_OPS_RESOLVER_SIZE>();

        if (s_resolver->AddEthosU() != kTfLiteOk) return -12;
    }

    s_model = tflite::GetModel(g_glucose_model);
    if (s_model == nullptr) return -13;
    if (s_model->version() != TFLITE_SCHEMA_VERSION) return -14;

    if (s_interpreter == nullptr)
    {
        size_t sz    = sizeof(tflite::MicroInterpreter);
        size_t align = alignof(tflite::MicroInterpreter);

        s_interpreter_storage = (uint8_t *)rt_malloc_align(sz, align);
        if (s_interpreter_storage == RT_NULL) return -15;

        s_interpreter = new (s_interpreter_storage)
            tflite::MicroInterpreter(s_model, *s_resolver,
                                     s_tensor_arena, GLUCOSE_TENSOR_ARENA_SIZE);
    }

    if (s_interpreter->AllocateTensors() != kTfLiteOk) return -16;

    s_arena_used = (uint32_t)s_interpreter->arena_used_bytes();

    if (s_interpreter->inputs_size() < 2) return -17;

    TfLiteTensor *in0 = s_interpreter->input(0);
    TfLiteTensor *in1 = s_interpreter->input(1);

    if (in0 && in0->dims->size >= 2 && in0->dims->data[1] == PPG_LEN)
    {
        s_input_ppg  = in0;
        s_input_scal = in1;
    }
    else if (in1 && in1->dims->size >= 2 && in1->dims->data[1] == PPG_LEN)
    {
        s_input_ppg  = in1;
        s_input_scal = in0;
    }
    else
    {
        return -18;
    }

    s_output = s_interpreter->output(0);
    if (s_output == nullptr) return -19;

    s_ppg_scale  = s_input_ppg->params.scale;
    s_ppg_zp     = s_input_ppg->params.zero_point;
    s_scal_scale = s_input_scal->params.scale;
    s_scal_zp    = s_input_scal->params.zero_point;
    s_out_scale  = s_output->params.scale;
    s_out_zp     = s_output->params.zero_point;

    if (s_ppg_scale == 0.0f || s_scal_scale == 0.0f || s_out_scale == 0.0f) return -20;

    s_s2_init_done = 1;
    rt_kprintf("[glucose_inference S3] >>> Init DONE, arena_used=%u, ready for invoke <<<\n",
               (unsigned)s_arena_used);
    return 0;
}

extern "C" int glucose_inference_is_ready(void)
{
    return s_s2_init_done;  /* S3: 暴露真实就绪状态,task 走 NPU 路径 */
}

extern "C" uint32_t glucose_inference_arena_used(void)
{
    return s_arena_used;
}

/* ------------------------------------------------------------------ */
/*  API: invoke (S3 完整实现)                                           */
/* ------------------------------------------------------------------ */

extern "C" int glucose_inference_invoke(const float ppg[128][2],
                                        const float scal[7],
                                        float *out_mmol)
{
    if (!s_s2_init_done || s_interpreter == nullptr) return -1;
    if (s_input_ppg == nullptr || s_input_scal == nullptr || s_output == nullptr) return -1;
    if (out_mmol == nullptr) return -1;

    /* NPU 运行时再检查一次(防中途掉电/故障) */
    if (!npu_is_ready()) return -1;

    if (s_ppg_scale == 0.0f || s_scal_scale == 0.0f || s_out_scale == 0.0f) return -2;

    /* 1. 量化 PPG [128][2] → int8 [256] */
    int8_t *ppg_data = s_input_ppg->data.int8;
    if (ppg_data == nullptr) return -2;

    for (int i = 0; i < PPG_LEN; i++)
    {
        ppg_data[i * PPG_CHANNELS + 0] =
            quantize_to_int8(ppg[i][0], s_ppg_scale, s_ppg_zp);
        ppg_data[i * PPG_CHANNELS + 1] =
            quantize_to_int8(ppg[i][1], s_ppg_scale, s_ppg_zp);
    }

    /* 2. 归一化 + 量化 SCAL [7] → int8 [7]
     *
     * ★ 归一化此前**整个缺失**（2026-07-17 修）—— 这是血糖估算失效的根因。
     *
     * 训练侧 glucose_training/scripts/model.py 的 normalize_scalars() 先把 7 维
     * 压到 [0,1] 再喂模型，其 docstring 明写"此函数定义训练-部署强契约，M55 端
     * 必须完全照搬此公式"。本文件此前只做 int8 量化、直接送原始物理量，契约没兑现。
     *
     * 后果（实测 tflite 标量输入 scale=0.0036/zp=-128，可表范围仅 [0, 0.919]）：
     *   HR=104 → 104/0.0036 = 28869 → clip 127
     *   ...7 维**全部**饱和成 127 → 标量分支输入恒为常量向量
     *   → 模型根本读不到传感器，HR 60 还是 120 输出都一样。
     *
     * 归一化必须放在这里而非调用方：契约的两端是"训练脚本"与"喂给 tflite 的
     * 张量"，紧挨量化才不会被下一个调用方绕过。
     */
    static const float SCAL_NORM_SUB[SCAL_DIM] = { 0.0f, 90.0f, 0.0f, 35.0f, 15.0f, 20.0f, 0.0f };
    static const float SCAL_NORM_DIV[SCAL_DIM] = { 120.0f, 10.0f, 5.0f, 5.0f, 20.0f, 70.0f, 120.0f };

    int8_t *scal_data = s_input_scal->data.int8;
    if (scal_data == nullptr) return -2;

    for (int i = 0; i < SCAL_DIM; i++)
    {
        float norm = (scal[i] - SCAL_NORM_SUB[i]) / SCAL_NORM_DIV[i];
        scal_data[i] = quantize_to_int8(norm, s_scal_scale, s_scal_zp);
    }

    /* 3. Invoke (阻塞,NPU 推理 ~20μs) */
    TfLiteStatus invoke_status = s_interpreter->Invoke();
    if (invoke_status != kTfLiteOk) return -3;

    /* 4. 反量化输出 + clamp 到生理范围 */
    int8_t *out_data = s_output->data.int8;
    if (out_data == nullptr) return -4;

    float out = dequantize_from_int8(out_data[0], s_out_scale, s_out_zp);
    if (out < GLUCOSE_MIN_MMOL) out = GLUCOSE_MIN_MMOL;
    if (out > GLUCOSE_MAX_MMOL) out = GLUCOSE_MAX_MMOL;

    *out_mmol = out;
    return 0;
}
