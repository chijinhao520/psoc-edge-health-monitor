/*
 * arrhythmia_test.cpp — MSH 命令 arrhythmia_test
 *
 * 用真实 MIT-BIH DS2 心拍(arrhythmia_test_data.h)做端到端 NPU 推理,验证:
 *   1. arrhythmia_inference_init 是否成功 + tensor_arena 实际占用
 *   2. NPU 逐拍预测类 == PC int8 tflite 预测类  → 训练-部署等价(最紧校验)
 *   3. NPU 逐拍预测类 == 真值                    → 真实可用性
 *   4. 单拍推理耗时
 *
 * 不依赖 AD8232 / IPC,板子 NPU 就绪后任意时刻可调。
 * 注:M55 MSH 当前禁用(UART2 留给 M33),MSH_CMD_EXPORT 仅留符号;
 *     也可在代码内显式调 arrhythmia_test()。
 *
 * 使用(M55 MSH 启用后): msh > arrhythmia_test
 */

#include <rtthread.h>

extern "C" {
#include "arrhythmia_inference.h"
#include "arrhythmia_test_data.h"
extern int npu_is_ready(void);
}

/* M55 rt_kprintf 不支持 %f,整数拆分打印 */
static void print_float(float val, int decimals)
{
    if (val < 0.0f) { rt_kprintf("-"); val = -val; }
    int int_part = (int)val;
    int scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    int frac = (int)((val - (float)int_part) * (float)scale + 0.5f);
    if (frac >= scale) { int_part++; frac -= scale; }
    rt_kprintf("%d.%0*d", int_part, decimals, frac);
}

static void arrhythmia_test(void)
{
    rt_kprintf("\n=== Arrhythmia Model NPU Inference Test ===\n");

    if (!npu_is_ready())
    {
        rt_kprintf("[ERROR] NPU not ready yet, wait ~3s after boot\n");
        return;
    }

    int init_ret = arrhythmia_inference_init();
    if (init_ret != 0)
    {
        rt_kprintf("[ERROR] arrhythmia_inference_init failed: %d\n", init_ret);
        return;
    }
    rt_kprintf("[INFO] Init OK, arena_used=%u bytes, %d test beats\n",
               (unsigned)arrhythmia_inference_arena_used(), ARR_TEST_N);

    rt_kprintf("\n  beat  true  ref(PC)  npu   conf    t(ms)  match\n");
    rt_kprintf("  ----  ----  -------  ----  -----   -----  -----\n");

    int match_ref  = 0;   /* NPU == PC int8 (等价) */
    int match_true = 0;   /* NPU == 真值 (可用性) */
    uint32_t total_ticks = 0;

    for (int b = 0; b < ARR_TEST_N; b++)
    {
        int   cls  = -1;
        float conf = 0.0f;

        uint32_t t0 = rt_tick_get();
        int ret = arrhythmia_inference_invoke(
            arr_test_ecg[b], arr_test_rr[b], &cls, &conf, RT_NULL);
        uint32_t t1 = rt_tick_get();

        if (ret != 0)
        {
            rt_kprintf("  [%d]  FAILED (ret=%d)\n", b, ret);
            return;
        }

        uint32_t dt = t1 - t0;
        total_ticks += dt;

        int ok_ref  = (cls == arr_test_ref[b]);
        int ok_true = (cls == arr_test_true[b]);
        if (ok_ref)  match_ref++;
        if (ok_true) match_true++;

        rt_kprintf("  [%d]   %s     %s      %s   ",
                   b,
                   arrhythmia_class_name(arr_test_true[b]),
                   arrhythmia_class_name(arr_test_ref[b]),
                   arrhythmia_class_name(cls));
        print_float(conf, 3);
        rt_kprintf("   %u      %s\n", (unsigned)dt, ok_ref ? "OK" : "XX");
    }

    rt_kprintf("\n  NPU vs PC-int8 (等价): %d/%d\n", match_ref, ARR_TEST_N);
    rt_kprintf("  NPU vs ground-truth   : %d/%d\n", match_true, ARR_TEST_N);
    rt_kprintf("  Avg time: %u ticks (~ms @1kHz)\n",
               (unsigned)(total_ticks / (ARR_TEST_N ? ARR_TEST_N : 1)));

    if (match_ref == ARR_TEST_N)
        rt_kprintf("\n>>> PASS: NPU 与 PC int8 逐拍一致,部署链路等价 <<<\n");
    else
        rt_kprintf("\n>>> WARN: NPU 与 PC int8 有 %d 拍不一致,查量化/前处理 <<<\n",
                   ARR_TEST_N - match_ref);
}

extern "C" {
MSH_CMD_EXPORT(arrhythmia_test, NPU arrhythmia model inference test);
}
