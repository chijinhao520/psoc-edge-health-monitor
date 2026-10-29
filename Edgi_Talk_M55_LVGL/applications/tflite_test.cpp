/*
 * TFLite Micro Hello World 验证测试
 *
 * 使用 sine 模型验证 TFLite Micro 框架在 RT-Thread + Cortex-M55 上能否正常推理。
 * 注册为 MSH 命令，串口输入 "tflite_test" 即可运行。
 *
 * 前置条件：
 *   1. 运行 gen_sine_model.py 生成 sine_model_data.h
 *   2. TFLite Micro 源码已放入 libraries/components/tflite-micro/
 *   3. menuconfig 已启用 RT_USING_CPLUSPLUS
 */

#include <rtthread.h>
#include <math.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "sine_model_data.h"

/* Tensor Arena — 模型推理的工作内存，sine 模型 8KB 绰绰有余 */
static uint8_t tensor_arena[8 * 1024];

/* M55 rt_kprintf 不支持 %f，用整数拆分打印浮点 */
static void print_float(float val, int decimals)
{
    if (val < 0.0f) { rt_kprintf("-"); val = -val; }
    int int_part = (int)val;
    int scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    int frac_part = (int)((val - int_part) * scale + 0.5f);
    if (frac_part >= scale) { int_part++; frac_part -= scale; }
    rt_kprintf("%d.%0*d", int_part, decimals, frac_part);
}

/*
 * tflite_test — MSH 命令入口
 *
 * 加载 sine 模型，对 [0, π] 区间几个点做推理，对比 sinf() 真值。
 */
static void tflite_test(void)
{
    rt_kprintf("=== TFLite Micro Hello World Test ===\n");

    /* 1. 加载模型 */
    const tflite::Model *model = tflite::GetModel(g_sine_model_data);
    if (model == nullptr)
    {
        rt_kprintf("[ERROR] Failed to load model\n");
        return;
    }
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        rt_kprintf("[ERROR] Model schema version %d != expected %d\n",
                   (int)model->version(), TFLITE_SCHEMA_VERSION);
        return;
    }
    rt_kprintf("Model loaded, schema version: %d\n", (int)model->version());

    /* 2. 注册算子 — sine 模型只用 FullyConnected + Relu */
    tflite::MicroMutableOpResolver<2> resolver;
    resolver.AddFullyConnected();

    /* 3. 创建解释器 */
    tflite::MicroInterpreter interpreter(model, resolver, tensor_arena,
                                         sizeof(tensor_arena));
    if (interpreter.AllocateTensors() != kTfLiteOk)
    {
        rt_kprintf("[ERROR] AllocateTensors failed\n");
        return;
    }

    /* 输入/输出 tensor 信息 */
    TfLiteTensor *input  = interpreter.input(0);
    TfLiteTensor *output = interpreter.output(0);
    rt_kprintf("Input:  type=%d, dims=[%d]\n", input->type, input->dims->data[0]);
    rt_kprintf("Output: type=%d, dims=[%d]\n", output->type, output->dims->data[0]);
    rt_kprintf("Arena used: %d bytes\n", (int)interpreter.arena_used_bytes());

    /* 4. 推理测试 */
    const float test_x[] = {0.0f, 0.5f, 1.0f, 1.57f, 2.0f, 3.14f};
    const int n_tests = sizeof(test_x) / sizeof(test_x[0]);

    rt_kprintf("\n  x        predict    expected   error\n");
    rt_kprintf("  ------   --------   --------   ------\n");

    float total_error = 0.0f;
    for (int i = 0; i < n_tests; i++)
    {
        input->data.f[0] = test_x[i];

        if (interpreter.Invoke() != kTfLiteOk)
        {
            rt_kprintf("[ERROR] Invoke failed at x=%d.%02d\n",
                       (int)test_x[i], (int)(test_x[i] * 100) % 100);
            return;
        }

        float y_pred = output->data.f[0];
        float y_true = sinf(test_x[i]);
        float error  = y_pred - y_true;
        total_error += error * error;

        rt_kprintf("  ");
        print_float(test_x[i], 2);
        rt_kprintf("     ");
        print_float(y_pred, 4);
        rt_kprintf("    ");
        print_float(y_true, 4);
        rt_kprintf("    ");
        if (error >= 0.0f) rt_kprintf("+");
        print_float(error, 4);
        rt_kprintf("\n");
    }

    float rmse = sqrtf(total_error / n_tests);
    rt_kprintf("\nRMSE: ");
    print_float(rmse, 4);
    rt_kprintf("\n");

    if (rmse < 0.1f)
        rt_kprintf(">>> PASS: TFLite Micro inference OK! <<<\n");
    else
        rt_kprintf(">>> WARN: RMSE too high, model may need retraining <<<\n");
}

/* 导出为 MSH 命令 */
extern "C" {
MSH_CMD_EXPORT(tflite_test, TFLite Micro sine model inference test);
}
