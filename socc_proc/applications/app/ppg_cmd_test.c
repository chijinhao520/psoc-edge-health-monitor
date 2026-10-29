/*
 * 反向 IPC（M55 按钮 → M33）诊断命令
 *
 * 反向通道是全项目唯一 M55 写 / M33 读的 IPC，D-Cache 方向也与其余相反
 * （M55 需 Clean，M33 无 cache 直读）。一旦按钮没反应，必须能分清是
 *   ① 用户没点 / ② M55 没写 / ③ 写了但 M33 读不到（cache 没刷）
 * 这三种情况，否则只能瞎猜。
 *
 * ppg_cmd_dump：直接打印 M33 视角看到的命令区原始内容。
 *   magic 不对   → M55 从未写过（按钮回调没跑，或地址对不上）
 *   magic 对但 seq 不涨 → M55 写了但没传过来（cache 未 clean）
 *   seq 在涨     → 通道通，问题在状态机
 */

#include <rtthread.h>
#include "ipc_shared_data.h"

static void ppg_cmd_dump(void)
{
    volatile ppg_cmd_ipc_t *c = ipc_get_cmd_ptr();

    __asm__ volatile("dsb sy" ::: "memory");

    rt_kprintf("PPG cmd IPC @ 0x%08X\n", (unsigned)IPC_CMD_ADDR);
    rt_kprintf("  magic = 0x%08X (expect 0x%08X) %s\n",
               (unsigned)c->magic, (unsigned)IPC_CMD_MAGIC,
               (c->magic == IPC_CMD_MAGIC) ? "OK" : "<- M55 never wrote");
    rt_kprintf("  seq   = %u\n", (unsigned)c->seq);
    rt_kprintf("  cmd   = %d %s\n", (int)c->cmd,
               (c->cmd == PPG_CMD_START_MEASURE) ? "(START_MEASURE)" : "");
}
MSH_CMD_EXPORT(ppg_cmd_dump, dump reverse IPC command area written by M55 button);

/*
 * 从串口发起一次测量，等价于按屏上的"开始测量"。
 *
 * 存在意义：调试测量链路时，屏幕按钮要求"人点按钮"与"人贴手指"跟"抓串口"
 * 三者时机对齐，来回试错代价很高。这个命令把触发从 UI 解耦出来，
 * 只需受试者保持手指贴合，调试者随时可发起。
 *
 * 直接写与 M55 按钮同一块命令 IPC（M33 自己写自己读，无 cache 问题：
 * CM33 无 D-Cache）。seq 递增语义与 M55 侧一致，ppg_task 分不出也不必分
 * 命令来自屏幕还是串口。
 */
static void ppg_measure(void)
{
    volatile ppg_cmd_ipc_t *c = ipc_get_cmd_ptr();
    uint32_t next;

    __asm__ volatile("dsb sy" ::: "memory");
    next = (c->magic == IPC_CMD_MAGIC) ? (c->seq + 1) : 1;

    c->cmd   = PPG_CMD_START_MEASURE;
    c->magic = IPC_CMD_MAGIC;
    c->seq   = next;
    __asm__ volatile("dsb sy" ::: "memory");

    rt_kprintf("measure requested from MSH (seq=%u), keep finger on sensor\n",
               (unsigned)next);
}
MSH_CMD_EXPORT(ppg_measure, trigger a PPG measurement session from serial (same as on-screen button));
