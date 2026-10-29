/*
 * glucose_test.cpp — MSH 命令 glucose_test
 *
 * 用固定输入做一次端到端 NPU 推理,验证:
 *   1. glucose_inference_init 是否成功
 *   2. tensor_arena 实际占用
 *   3. NPU Invoke 是否返回 OK
 *   4. 输出反量化后是否在生理范围
 *   5. 单次推理耗时
 *
 * 不依赖 IPC 数据,可在板子初始化完成后任意时刻调用。
 *
 * 注:M55 MSH 当前禁用(rtconfig.h:68-81 注释,UART2 留给 M33)。
 *     MSH_CMD_EXPORT 仅在 elf 留符号,未来启用 M55 MSH 后直接可用。
 *     也可在代码内显式调 glucose_test() 函数。
 *
 * 使用 (M55 MSH 启用后):
 *   msh > glucose_test
 */

#include <rtthread.h>
#include <math.h>

extern "C" {
#include "glucose_inference.h"
extern int npu_is_ready(void);
}

/* M55 rt_kprintf 不支持 %f,用整数拆分打印浮点 */
static void print_float(float val, int decimals)
{
    if (val < 0.0f) { rt_kprintf("-"); val = -val; }
    int int_part = (int)val;
    int scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    int frac_part = (int)((val - (float)int_part) * (float)scale + 0.5f);
    if (frac_part >= scale) { int_part++; frac_part -= scale; }
    rt_kprintf("%d.%0*d", int_part, decimals, frac_part);
}

/*
 * glucose_test — 用固定输入跑 5 次推理,打印诊断信息
 */
static void glucose_test(void)
{
    rt_kprintf("\n=== Glucose Model NPU Inference Test ===\n");

    if (!npu_is_ready())
    {
        rt_kprintf("[ERROR] NPU not ready yet, please wait ~3s after boot\n");
        return;
    }

    int init_ret = glucose_inference_init();
    if (init_ret != 0)
    {
        rt_kprintf("[ERROR] glucose_inference_init failed: %d\n", init_ret);
        return;
    }

    rt_kprintf("[INFO] Init OK, arena_used=%u bytes\n",
               (unsigned)glucose_inference_arena_used());

    /* 准备固定输入:0.5 基线 + 微小正弦扰动模拟脉搏 */
    static float ppg[128][2];
    for (int i = 0; i < 128; i++)
    {
        float t = (float)i / 128.0f;
        float pulse = 0.05f * sinf(2.0f * 3.14159265f * t * 2.0f);
        ppg[i][0] = 0.5f + pulse;
        ppg[i][1] = 0.5f + pulse * 0.8f;
    }

    /* 中位生理标量 */
    float scal[7] = { 75.0f, 98.0f, 1.0f, 40.0f, 36.5f, 25.0f, 55.0f };

    rt_kprintf("\n  iter   glucose      time(ms)\n");
    rt_kprintf("  ----   --------     --------\n");

    float total = 0.0f;
    uint32_t total_ticks = 0;

    for (int it = 0; it < 5; it++)
    {
        float out = 0.0f;
        uint32_t t0 = rt_tick_get();
        int ret = glucose_inference_invoke(ppg, scal, &out);
        uint32_t t1 = rt_tick_get();

        if (ret != 0)
        {
            rt_kprintf("  [%d]   FAILED (ret=%d)\n", it, ret);
            return;
        }

        uint32_t dt = t1 - t0;
        total_ticks += dt;
        total += out;

        rt_kprintf("  [%d]   ", it);
        print_float(out, 3);
        rt_kprintf("        %u\n", (unsigned)dt);
    }

    float avg = total / 5.0f;
    uint32_t avg_ticks = total_ticks / 5;

    rt_kprintf("\n  Avg glucose: ");
    print_float(avg, 3);
    rt_kprintf(" mmol/L\n");
    rt_kprintf("  Avg time:    %u ticks (~%u ms @ 1kHz)\n",
               (unsigned)avg_ticks, (unsigned)avg_ticks);

    if (avg >= 2.0f && avg <= 20.0f)
    {
        rt_kprintf("\n>>> PASS: NPU inference in physiological range <<<\n");
    }
    else
    {
        rt_kprintf("\n>>> WARN: output out of [2,20] mmol/L range <<<\n");
    }
}

/* MSH 命令导出(M55 MSH 启用后可用,当前是死符号无害) */
extern "C" {
MSH_CMD_EXPORT(glucose_test, NPU glucose model inference test);
}
