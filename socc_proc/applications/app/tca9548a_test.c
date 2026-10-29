/*
 * TCA9548A 控制寄存器读写自测（MSH 命令，按需手动执行）
 *
 * 板上暂无下游传感器模块时，用这个验证 mux 本体的寄存器级通信：
 * 依次写入单通道/全通道/关闭几种位图，每次写完立即回读比对。
 */

#include <rtthread.h>
#include "../sensor/tca9548a.h"

static void tca9548a_selftest(void)
{
    static const rt_uint8_t patterns[] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, /* 单通道 CH0-CH7 */
        0xFF,                                            /* 全部通道同时选通 */
        0x00                                              /* 全部关闭 */
    };
    int total = sizeof(patterns) / sizeof(patterns[0]);
    tca9548a_device_t dev;
    int i, pass = 0;

    /*
     * 用进程内共享单例，不能自己 tca9548a_init() 建私有实例——ppg_task 也在
     * 持续用同一块 mux 轮询 CH0/CH1，必须共享同一把锁才不会互相踩通道。
     * 这个自测本身就是要往控制寄存器写任意 pattern，运行期间 ppg_task 会
     * 短暂等锁（正常现象），但不会读到被半路改掉的通道。
     */
    dev = tca9548a_shared();
    if (dev == RT_NULL)
    {
        rt_kprintf("TCA9548A shared mux unavailable, abort self-test\n");
        return;
    }

    rt_kprintf("TCA9548A control register self-test (write pattern -> readback compare):\n");
    for (i = 0; i < total; i++)
    {
        rt_uint8_t readback = 0xAA;  /* 哨兵值，确认确实被覆写而非残留旧值 */
        rt_err_t werr, rerr;

        tca9548a_lock(dev);
        werr = tca9548a_set_channel(dev, patterns[i]);
        rerr = tca9548a_get_channel(dev, &readback);
        tca9548a_unlock(dev);

        if (werr == RT_EOK && rerr == RT_EOK && readback == patterns[i])
        {
            rt_kprintf("  [PASS] wrote 0x%02X, read back 0x%02X\n", patterns[i], readback);
            pass++;
        }
        else
        {
            rt_kprintf("  [FAIL] wrote 0x%02X, read back 0x%02X (werr=%d rerr=%d)\n",
                       patterns[i], readback, werr, rerr);
        }
    }

    rt_kprintf("TCA9548A self-test done: %d/%d passed\n", pass, total);
    /* 不 deinit 共享 mux —— ppg_task 等其他使用者还依赖它存活 */
}
MSH_CMD_EXPORT(tca9548a_selftest, TCA9548A control register read/write self-test);

/*
 * 逐通道扫描下游设备：每次只选通一个通道，在该通道上跑一次完整地址扫描。
 * 用于验证 mux 的通道电气隔离是否正常——期望只有实际接了传感器的那个
 * 通道能扫到对应地址，其余通道应为空。
 */
static void tca9548a_ch_scan(void)
{
    tca9548a_device_t dev;
    int ch;

    /* 同 tca9548a_selftest：必须用共享单例，否则跟 ppg_task 各建一把互不相干的锁 */
    dev = tca9548a_shared();
    if (dev == RT_NULL)
    {
        rt_kprintf("TCA9548A shared mux unavailable, abort channel scan\n");
        return;
    }

    for (ch = 0; ch < 8; ch++)
    {
        rt_uint8_t buf = 0;
        int addr;
        int count = 0;

        tca9548a_lock(dev);
        tca9548a_select_channel(dev, ch);
        rt_kprintf("-- CH%d --\n", ch);
        for (addr = 0x08; addr < 0x78; addr++)
        {
            if (rt_i2c_master_recv(dev->i2c, addr, 0, &buf, 1) == 1)
            {
                rt_kprintf("  [0x%02X] ACK\n", addr);
                count++;
            }
        }
        tca9548a_unlock(dev);

        if (count == 0)
        {
            rt_kprintf("  (no device)\n");
        }
    }
    /* 不 deinit 共享 mux —— ppg_task 等其他使用者还依赖它存活 */
}
MSH_CMD_EXPORT(tca9548a_ch_scan, scan downstream devices on each TCA9548A channel);
