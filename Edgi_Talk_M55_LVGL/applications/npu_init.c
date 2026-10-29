/*
 * Ethos-U55 NPU 初始化与 RT-Thread 适配层
 *
 * 功能：
 *   1. 实现 ethosu_driver 需要的弱函数（mutex/semaphore/D-Cache）
 *   2. NPU 驱动初始化（ethosu_init）
 *   3. NPU IRQ 注册
 *
 * 基地址：0x44600000（Non-Secure，来自 cy_device_headers_ns.h U550_BASE）
 * IRQ：mxu55_interrupt_npu_IRQn（153 Active / 38 DeepSleep）
 */

#include <rtthread.h>
#include <ethosu_driver.h>

/* PSoC E84 NPU 寄存器基地址（Non-Secure 模式） */
#define NPU_BASE_ADDRESS    ((void *)0x44600000UL)

/* 全局 NPU 驱动实例 */
static struct ethosu_driver npu_drv;

/*===========================================================================
 * RT-Thread 适配层：覆盖 ethosu_driver.c 中的 weak 函数
 *===========================================================================*/

/* ---- D-Cache 维护 ---- */
void ethosu_flush_dcache(uint32_t *p, size_t bytes)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (bytes > 0)
    {
        SCB_CleanDCache_by_Addr(p, (int32_t)bytes);
    }
#else
    (void)p;
    (void)bytes;
#endif
}

void ethosu_invalidate_dcache(uint32_t *p, size_t bytes)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (bytes > 0)
    {
        SCB_InvalidateDCache_by_Addr(p, (int32_t)bytes);
    }
#else
    (void)p;
    (void)bytes;
#endif
}

/* ---- 互斥锁 ---- */
void *ethosu_mutex_create(void)
{
    rt_mutex_t mutex = rt_mutex_create("npu_mtx", RT_IPC_FLAG_PRIO);
    return (void *)mutex;
}

void ethosu_mutex_destroy(void *mutex)
{
    if (mutex)
    {
        rt_mutex_delete((rt_mutex_t)mutex);
    }
}

int ethosu_mutex_lock(void *mutex)
{
    if (mutex == RT_NULL) return -1;
    rt_err_t ret = rt_mutex_take((rt_mutex_t)mutex, RT_WAITING_FOREVER);
    return (ret == RT_EOK) ? 0 : -1;
}

int ethosu_mutex_unlock(void *mutex)
{
    if (mutex == RT_NULL) return -1;
    rt_err_t ret = rt_mutex_release((rt_mutex_t)mutex);
    return (ret == RT_EOK) ? 0 : -1;
}

/* ---- 信号量 ---- */
void *ethosu_semaphore_create(void)
{
    rt_sem_t sem = rt_sem_create("npu_sem", 0, RT_IPC_FLAG_PRIO);
    return (void *)sem;
}

void ethosu_semaphore_destroy(void *sem)
{
    if (sem)
    {
        rt_sem_delete((rt_sem_t)sem);
    }
}

int ethosu_semaphore_take(void *sem, uint64_t timeout)
{
    if (sem == RT_NULL) return -1;

    rt_int32_t ticks;
    if (timeout == ETHOSU_SEMAPHORE_WAIT_FOREVER)
    {
        ticks = RT_WAITING_FOREVER;
    }
    else
    {
        /* timeout 单位是 us，转换为 tick */
        ticks = (rt_int32_t)rt_tick_from_millisecond((uint32_t)(timeout / 1000));
        if (ticks == 0) ticks = 1;
    }

    rt_err_t ret = rt_sem_take((rt_sem_t)sem, ticks);
    return (ret == RT_EOK) ? 0 : -1;
}

int ethosu_semaphore_give(void *sem)
{
    if (sem == RT_NULL) return -1;
    rt_err_t ret = rt_sem_release((rt_sem_t)sem);
    return (ret == RT_EOK) ? 0 : -1;
}

/*===========================================================================
 * NPU IRQ Handler
 *
 * 注意：ns_start_pse84.c 的向量表是硬编码数组，所有外设中断槽位都字面写死
 * 指向通用 InterruptHandler，不支持按函数名弱符号覆盖——这个函数必须配合
 * npu_hw_init 里的 NVIC_SetVector 手动挂载才会被真正调用，见下方注册处。
 *===========================================================================*/
void mxu55_interrupt_npu_Handler(void)
{
    rt_interrupt_enter();
    ethosu_irq_handler(&npu_drv);
    rt_interrupt_leave();
}

/*===========================================================================
 * NPU 初始化（延迟线程，开机后自动执行）
 *
 * 为什么用线程而不是 INIT_DEVICE_EXPORT：
 *   NPU 时钟可能需要系统完全启动后才可用，延迟 3s 确保安全。
 *   如果 ethosu_init 失败，不影响其他功能（LVGL/WiFi/传感器）。
 *===========================================================================*/
#include "system_edge.h"

static volatile int npu_ready = 0;  /* 供 glucose_task 查询 */

int npu_is_ready(void)
{
    return npu_ready;
}

struct ethosu_driver *npu_get_driver(void)
{
    return npu_ready ? &npu_drv : RT_NULL;
}

static void npu_init_thread_entry(void *param)
{
    (void)param;
    int ret;

    /* 等待系统稳定 */
    rt_thread_mdelay(3000);

    /* 使能 NPU：时钟 + PPU 电源域 + PDCM 依赖（BSP 官方 API） */
    Cy_SysEnableU55(true);
    rt_kprintf("[NPU] Cy_SysEnableU55(true) done (clock + PPU + PDCM)\n");

    rt_thread_mdelay(10);  /* 等电源稳定 */

    /* 探测 NPU 寄存器是否可读 */
    volatile uint32_t *npu_base = (volatile uint32_t *)NPU_BASE_ADDRESS;
    rt_kprintf("[NPU] REG[0x00](ID)=0x%08X, REG[0x04](STATUS)=0x%08X\n",
               npu_base[0], npu_base[1]);

    rt_kprintf("[NPU] Calling ethosu_init ...\n");

    /* 初始化 NPU 驱动 */
    ret = ethosu_init(&npu_drv,
                      NPU_BASE_ADDRESS,
                      NULL,   /* fast_memory: U55 不用 */
                      0,      /* fast_memory_size */
                      0,      /* secure_enable = 0 (Non-Secure) */
                      0);     /* privilege_enable = 0 */

    if (ret != 0)
    {
        rt_kprintf("[NPU] ethosu_init FAILED (%d), NPU unavailable\n", ret);
        return;
    }

    /*
     * 注册 NPU 中断。
     *
     * ns_start_pse84.c 的向量表 __ns_vector_table[] 是一整块硬编码数组，
     * 所有外设中断槽位全部字面写死指向同一个通用 InterruptHandler（bkpt+死循环），
     * 不是每个中断有独立弱符号可覆盖——mxu55_interrupt_npu_Handler 单靠函数名
     * 匹配根本挂不上向量表，NVIC_EnableIRQ 只是使能了这个中断号，真正触发时
     * 向量到的还是那个通用 trap。必须用 NVIC_SetVector 在运行时手动改写
     * RAM 向量表（Reset_Handler 里已经把 __ns_vector_table 拷到
     * __ns_vector_table_rw 并让 SCB->VTOR 指过去，是可写的）才能真正生效。
     */
    NVIC_SetVector(mxu55_interrupt_npu_IRQn, (uint32_t)mxu55_interrupt_npu_Handler);
    NVIC_SetPriority(mxu55_interrupt_npu_IRQn, 5);
    NVIC_EnableIRQ(mxu55_interrupt_npu_IRQn);

    /* 打印硬件信息 */
    struct ethosu_hw_info hw_info;
    ethosu_get_hw_info(&npu_drv, &hw_info);
    rt_kprintf("[NPU] Ethos-U55 init OK, version: %d.%d, product: %d, arch: %d.%d.%d\n",
               hw_info.version.version_major,
               hw_info.version.version_minor,
               hw_info.version.product_major,
               hw_info.version.arch_major_rev,
               hw_info.version.arch_minor_rev,
               hw_info.version.arch_patch_rev);

    npu_ready = 1;
}

static int npu_init_startup(void)
{
    rt_thread_t tid = rt_thread_create("npu_init", npu_init_thread_entry, RT_NULL,
                                       2048, 18, 10);
    if (tid)
    {
        rt_thread_startup(tid);
    }
    return 0;
}
INIT_APP_EXPORT(npu_init_startup);
