/**
 * @file  fault_dump.c
 * @brief M55 HardFault 诊断增强 —— 在 RT-Thread 默认 PC/LR dump 之外，补打印
 *        CFSR/HFSR/BFAR/MMFAR 的 fault 类型解码。
 *
 * 背景（2026-06-17）：本工程 RT_USING_FINSH 关闭（UART2 留给 M33），导致
 * cpuport.c 里 CFSR 解码的 hard_fault_track() 被 `#ifdef RT_USING_FINSH`
 * 编译掉。fault 时 cpuport.c 默认 handler 仍会打印 PC/LR/r0-r12（那段在
 * FINSH 之外，无条件），但**看不到 fault 类型** —— 而 fault 类型恰恰是
 * 定位 LVGL HardFault 根因的关键证据。
 *
 * 本文件用 RT-Thread 官方 rt_hw_exception_install() 机制（非侵入，不改
 * RT-Thread libcpu 源码）补全这段信息，并加 [M55FAULT] 前缀，方便从 M33
 * 共享的 uart2 日志里筛出来。
 *
 * hook 返回非 RT_EOK → 让 cpuport.c 默认 handler 继续打印 PC/LR 并 while(1)
 * 停机（fault 后绝不返回出错点）。
 *
 * ── 诊断判读（LVGL HardFault 根因二分）─────────────────────────────
 *   UFSR.NOCP / UNDEFINSTR / INVSTATE  → FPU/指令集问题。强烈印证
 *        "编译目标 -mcpu=cortex-m7（Armv7e-M）vs 实际硬件 Cortex-M55
 *         （Armv8.1-M）不匹配" 的假设（LVGL 线程浮点密集，最先暴露）。
 *   UFSR.UNALIGNED                     → 未对齐访问（GCC13.3 优化暴露）。
 *   UFSR.STKOF                         → 栈溢出（LVGL 线程 24KB）。
 *   BFSR + BFAR                        → 坏指针 / NULL 解引用（LVGL 内部 UB）。
 *   MFSR + MMFAR                       → MPU 越权。
 *
 * 注意：本 hook 只在 fault **之后**执行，不改变正常路径时序，故**无观察者
 * 效应**（区别于面板 DSI 命令间隔那个 timing bug —— 那个是 print 耗时改变
 * 了 fault 之前的执行节奏）。
 */

#include <rtthread.h>

/* cpuport.c 提供；rthw.h 声明，这里显式 extern 以免头文件依赖 */
extern void rt_hw_exception_install(rt_err_t (*exception_handle)(void *context));

#define REG32(addr)     (*(volatile rt_uint32_t *)(addr))

/* System Control Block fault status registers (Armv7-M / Armv8-M 通用地址) */
#define SCB_CFSR_ADDR   0xE000ED28UL   /* Configurable Fault Status (MMFSR|BFSR|UFSR) */
#define SCB_HFSR_ADDR   0xE000ED2CUL   /* HardFault Status */
#define SCB_MMFAR_ADDR  0xE000ED34UL   /* MemManage Fault Address */
#define SCB_BFAR_ADDR   0xE000ED38UL   /* BusFault Address */

static rt_err_t fault_diag_hook(void *context)
{
    rt_uint32_t cfsr  = REG32(SCB_CFSR_ADDR);
    rt_uint32_t hfsr  = REG32(SCB_HFSR_ADDR);
    rt_uint32_t mmfsr = (cfsr >>  0) & 0xFFu;
    rt_uint32_t bfsr  = (cfsr >>  8) & 0xFFu;
    rt_uint32_t ufsr  = (cfsr >> 16) & 0xFFFFu;

    (void)context;   /* PC/LR 由 cpuport.c 默认 handler 紧随其后打印 */

    rt_kprintf("\n[M55FAULT] ======== HardFault diag ========\n");
    rt_kprintf("[M55FAULT] CFSR=0x%08x HFSR=0x%08x\n", cfsr, hfsr);

    if (ufsr)
    {
        rt_kprintf("[M55FAULT] UsageFault UFSR=0x%04x:", ufsr);
        if (ufsr & (1u << 0)) rt_kprintf(" UNDEFINSTR");
        if (ufsr & (1u << 1)) rt_kprintf(" INVSTATE");
        if (ufsr & (1u << 2)) rt_kprintf(" INVPC");
        if (ufsr & (1u << 3)) rt_kprintf(" NOCP");        /* FPU/协处理器未使能 */
        if (ufsr & (1u << 4)) rt_kprintf(" STKOF");       /* Armv8-M 栈溢出 */
        if (ufsr & (1u << 8)) rt_kprintf(" UNALIGNED");
        if (ufsr & (1u << 9)) rt_kprintf(" DIVBYZERO");
        rt_kprintf("\n");
    }
    if (bfsr)
    {
        rt_kprintf("[M55FAULT] BusFault  BFSR=0x%02x:", bfsr);
        if (bfsr & (1u << 0)) rt_kprintf(" IBUSERR");
        if (bfsr & (1u << 1)) rt_kprintf(" PRECISERR");
        if (bfsr & (1u << 2)) rt_kprintf(" IMPRECISERR");
        if (bfsr & (1u << 3)) rt_kprintf(" UNSTKERR");
        if (bfsr & (1u << 4)) rt_kprintf(" STKERR");
        if (bfsr & (1u << 5)) rt_kprintf(" LSPERR");
        if (bfsr & (1u << 7)) rt_kprintf(" BFARVALID BFAR=0x%08x", REG32(SCB_BFAR_ADDR));
        rt_kprintf("\n");
    }
    if (mmfsr)
    {
        rt_kprintf("[M55FAULT] MemManage MMFSR=0x%02x:", mmfsr);
        if (mmfsr & (1u << 0)) rt_kprintf(" IACCVIOL");
        if (mmfsr & (1u << 1)) rt_kprintf(" DACCVIOL");
        if (mmfsr & (1u << 3)) rt_kprintf(" MUNSTKERR");
        if (mmfsr & (1u << 4)) rt_kprintf(" MSTKERR");
        if (mmfsr & (1u << 5)) rt_kprintf(" MLSPERR");
        if (mmfsr & (1u << 7)) rt_kprintf(" MMARVALID MMFAR=0x%08x", REG32(SCB_MMFAR_ADDR));
        rt_kprintf("\n");
    }

    if (hfsr & (1u << 30)) rt_kprintf("[M55FAULT] HFSR.FORCED  (升级自上面的配置类 fault)\n");
    if (hfsr & (1u <<  1)) rt_kprintf("[M55FAULT] HFSR.VECTTBL (向量取指失败)\n");

    rt_kprintf("[M55FAULT] thread: %s\n",
               rt_thread_self() ? rt_thread_self()->parent.name : "(handler/none)");
    rt_kprintf("[M55FAULT] ==== 下面 pc/lr 用 arm-none-eabi-addr2line 反查源码行 ====\n");

    return -RT_ERROR;   /* 非 RT_EOK：让默认 handler 继续打印 PC/LR 并停机 */
}

static int fault_diag_init(void)
{
    rt_hw_exception_install(fault_diag_hook);
    return 0;
}
/* BOARD 阶段（调度器启动前）装好，早于一切线程，可捕获所有后续 fault。
 * 仅存指针，不调用 rt_thread_mdelay，符合 INIT_BOARD_EXPORT 约束。 */
INIT_BOARD_EXPORT(fault_diag_init);
