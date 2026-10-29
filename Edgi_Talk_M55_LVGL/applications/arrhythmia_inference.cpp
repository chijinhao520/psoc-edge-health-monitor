/*
 * arrhythmia_inference.cpp — 心律不齐 NPU 推理(TFLite Micro + Ethos-U55)
 *
 * 移植自 glucose_inference.cpp 同套模式(memory 2026-socc-tflite-port-pattern):
 *   - 全部 buffer 走 heap (m55_data_secondary 1.4MB),DTCM 不动
 *   - placement new 构造 resolver/interpreter,避免 C++ magic statics fault
 *   - __attribute__((used)) 防 g_arrhythmia_model 被 gc-sections strip
 *
 * 与 glucose 的差别:
 *   - 双输入 ecg[1,256,1] + rr[1,3](按 dims[1]==256 区分主/辅,可能 swap)
 *   - 输出是 [1,4] 分类 softmax → 反量化取 argmax,而非单值回归
 */

#include <rtthread.h>
#include <stdint.h>
#include <math.h>
#include <new>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "arrhythmia_model.h"

extern "C" {
#include "arrhythmia_inference.h"
extern int npu_is_ready(void);
}

#define ARR_TENSOR_ARENA_SIZE  (32 * 1024)   /* Vela SRAM ~10.7KiB,留余量;实测后可缩 */
#define ARR_OPS_RESOLVER_SIZE  2

namespace {

uint8_t *s_tensor_arena        = nullptr;
uint8_t *s_resolver_storage    = nullptr;
uint8_t *s_interpreter_storage = nullptr;

tflite::MicroMutableOpResolver<ARR_OPS_RESOLVER_SIZE> *s_resolver    = nullptr;
tflite::MicroInterpreter                              *s_interpreter = nullptr;

const tflite::Model *s_model     = nullptr;
TfLiteTensor        *s_input_ecg = nullptr;
TfLiteTensor        *s_input_rr  = nullptr;
TfLiteTensor        *s_output    = nullptr;

float   s_ecg_scale = 0.0f;  int32_t s_ecg_zp = 0;
float   s_rr_scale  = 0.0f;  int32_t s_rr_zp  = 0;
float   s_out_scale = 0.0f;  int32_t s_out_zp = 0;

uint32_t s_arena_used = 0;
int      s_init_done  = 0;

__attribute__((used))
const uint8_t *const s_model_anchor = g_arrhythmia_model;

const char *const k_class_names[ARR_N_CLASS] = { "N", "S", "V", "F" };

} // anonymous namespace

static inline int8_t quantize_to_int8(float f, float scale, int32_t zp)
{
    int32_t q = (int32_t)lroundf(f / scale) + zp;
    if (q < -128) q = -128;
    if (q >  127) q =  127;
    return (int8_t)q;
}

static inline float dequantize_from_int8(int8_t q, float scale, int32_t zp)
{
    return ((float)q - (float)zp) * scale;
}

extern "C" const char *arrhythmia_class_name(int cls)
{
    if (cls < 0 || cls >= ARR_N_CLASS) return "?";
    return k_class_names[cls];
}

/* ------------------------------------------------------------------ */
/*  init                                                               */
/* ------------------------------------------------------------------ */
extern "C" int arrhythmia_inference_init(void)
{
    if (s_init_done) return 0;

    rt_kprintf("[arrhythmia_inference] init enter\n");

    if (!npu_is_ready())
    {
        rt_kprintf("[arrhythmia_inference] NPU not ready\n");
        return -1;
    }

    if (s_tensor_arena == RT_NULL)
    {
        s_tensor_arena = (uint8_t *)rt_malloc_align(ARR_TENSOR_ARENA_SIZE, 16);
        if (s_tensor_arena == RT_NULL) return -10;
    }

    if (s_resolver == nullptr)
    {
        size_t sz    = sizeof(tflite::MicroMutableOpResolver<ARR_OPS_RESOLVER_SIZE>);
        size_t align = alignof(tflite::MicroMutableOpResolver<ARR_OPS_RESOLVER_SIZE>);

        s_resolver_storage = (uint8_t *)rt_malloc_align(sz, align);
        if (s_resolver_storage == RT_NULL) return -11;

        s_resolver = new (s_resolver_storage)
            tflite::MicroMutableOpResolver<ARR_OPS_RESOLVER_SIZE>();

        if (s_resolver->AddEthosU() != kTfLiteOk) return -12;
    }

    s_model = tflite::GetModel(g_arrhythmia_model);
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
                                     s_tensor_arena, ARR_TENSOR_ARENA_SIZE);
    }

    if (s_interpreter->AllocateTensors() != kTfLiteOk) return -16;
    s_arena_used = (uint32_t)s_interpreter->arena_used_bytes();

    if (s_interpreter->inputs_size() < 2) return -17;

    TfLiteTensor *in0 = s_interpreter->input(0);
    TfLiteTensor *in1 = s_interpreter->input(1);

    /* 按 dims[1]==256 判主输入(ECG),顺序可能 swap */
    if (in0 && in0->dims->size >= 2 && in0->dims->data[1] == ARR_ECG_LEN)
    {
        s_input_ecg = in0;
        s_input_rr  = in1;
    }
    else if (in1 && in1->dims->size >= 2 && in1->dims->data[1] == ARR_ECG_LEN)
    {
        s_input_ecg = in1;
        s_input_rr  = in0;
    }
    else
    {
        return -18;
    }

    s_output = s_interpreter->output(0);
    if (s_output == nullptr) return -19;

    s_ecg_scale = s_input_ecg->params.scale;
    s_ecg_zp    = s_input_ecg->params.zero_point;
    s_rr_scale  = s_input_rr->params.scale;
    s_rr_zp     = s_input_rr->params.zero_point;
    s_out_scale = s_output->params.scale;
    s_out_zp    = s_output->params.zero_point;

    if (s_ecg_scale == 0.0f || s_rr_scale == 0.0f || s_out_scale == 0.0f) return -20;

    s_init_done = 1;
    rt_kprintf("[arrhythmia_inference] >>> Init DONE, arena_used=%u, classes=4 <<<\n",
               (unsigned)s_arena_used);
    return 0;
}

extern "C" int arrhythmia_inference_is_ready(void)
{
    return s_init_done;
}

extern "C" uint32_t arrhythmia_inference_arena_used(void)
{
    return s_arena_used;
}

/* ------------------------------------------------------------------ */
/*  invoke                                                             */
/* ------------------------------------------------------------------ */
extern "C" int arrhythmia_inference_invoke(const float ecg[ARR_ECG_LEN],
                                           const float rr[ARR_RR_DIM],
                                           int *out_class,
                                           float *out_conf,
                                           float out_probs[ARR_N_CLASS])
{
    if (!s_init_done || s_interpreter == nullptr) return -1;
    if (s_input_ecg == nullptr || s_input_rr == nullptr || s_output == nullptr) return -1;
    if (ecg == nullptr || rr == nullptr || out_class == nullptr) return -1;
    if (!npu_is_ready()) return -1;
    if (s_ecg_scale == 0.0f || s_rr_scale == 0.0f || s_out_scale == 0.0f) return -2;

    /* 1. 量化 ECG [256] → int8 */
    int8_t *ecg_data = s_input_ecg->data.int8;
    if (ecg_data == nullptr) return -2;
    for (int i = 0; i < ARR_ECG_LEN; i++)
        ecg_data[i] = quantize_to_int8(ecg[i], s_ecg_scale, s_ecg_zp);

    /* 2. 量化 R-R [3] → int8 */
    int8_t *rr_data = s_input_rr->data.int8;
    if (rr_data == nullptr) return -2;
    for (int i = 0; i < ARR_RR_DIM; i++)
        rr_data[i] = quantize_to_int8(rr[i], s_rr_scale, s_rr_zp);

    /* 3. Invoke (NPU,~微秒级) */
    if (s_interpreter->Invoke() != kTfLiteOk) return -3;

    /* 4. 反量化 4 类 softmax 概率 + argmax */
    int8_t *od = s_output->data.int8;
    if (od == nullptr) return -3;

    int   best_i = 0;
    float best_p = -1.0f;
    for (int c = 0; c < ARR_N_CLASS; c++)
    {
        float p = dequantize_from_int8(od[c], s_out_scale, s_out_zp);
        if (out_probs) out_probs[c] = p;
        if (p > best_p) { best_p = p; best_i = c; }
    }

    *out_class = best_i;
    if (out_conf) *out_conf = best_p;
    return 0;
}
