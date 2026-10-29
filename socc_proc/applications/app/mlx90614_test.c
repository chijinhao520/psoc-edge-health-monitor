/*
 * MLX90614 单独读取诊断（MSH 命令）
 *
 * 目的：把"总线竞争/被抢占"与"器件本身不吐数据"一刀切开。
 *
 * mlx90614_task 的失败发生在 ppg_task 高速轮询的背景下，无法判断到底是
 * 抢总线抢输了，还是器件根本不响应数据阶段。本命令在 MSH 线程里连续读
 * 若干次并打印原始字节 —— 它同样要跟 ppg 抢锁，但可以对比：
 *   - 若这里也全 0xFF  → 与竞争无关，器件的数据阶段确实不响应
 *   - 若这里能读出数   → 竞争/时序问题，方向在调度而非硬件
 *
 * 打印原始 3 字节（DataL/DataH/PEC）而非只报 PEC 失败：
 * 全 0xFF = 总线浮空无响应；部分正确 = 时序被打断。两者根因完全不同。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "../sensor/mlx90614.h"
#include "../sensor/tca9548a.h"

#define MLX_MUX_CHANNEL  2
#define MLX90614_ADDR    0x5A

/* 直接发一次 SMBus read-word，把原始字节暴露出来（驱动内部只报 PEC 结果） */
static int mlx_raw_read(struct rt_i2c_bus_device *bus, rt_uint8_t cmd,
                        rt_uint8_t *buf3)
{
    rt_uint8_t c = cmd;
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = MLX90614_ADDR;
    msgs[0].flags = RT_I2C_WR | RT_I2C_NO_STOP;   /* NO_STOP → Repeated Start */
    msgs[0].buf   = &c;
    msgs[0].len   = 1;

    msgs[1].addr  = MLX90614_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf3;
    msgs[1].len   = 3;

    return (rt_i2c_transfer(bus, msgs, 2) == 2) ? 0 : -1;
}

static void mlx_diag(void)
{
    tca9548a_device_t mux;
    struct rt_i2c_bus_device *bus;
    int i;

    bus = rt_i2c_bus_device_find("i2c1");
    if (bus == RT_NULL)
    {
        rt_kprintf("i2c1 not found\n");
        return;
    }

    mux = tca9548a_shared();
    if (mux == RT_NULL)
    {
        rt_kprintf("mux unavailable\n");
        return;
    }

    rt_kprintf("--- MLX90614 raw read x8 (CH%d, addr 0x%02X) ---\n",
               MLX_MUX_CHANNEL, MLX90614_ADDR);
    rt_kprintf("all-0xFF => device silent in data phase (not a timing issue)\n");

    for (i = 0; i < 8; i++)
    {
        rt_uint8_t buf[3] = {0, 0, 0};
        rt_uint8_t probe  = 0;
        int rc_obj;
        int rc_addr;

        tca9548a_lock(mux);
        tca9548a_select_channel(mux, MLX_MUX_CHANNEL);

        /* 先做一次纯地址探测(1 字节)：它在扫描里是能 ACK 的，作为对照 */
        rc_addr = (rt_i2c_master_recv(bus, MLX90614_ADDR, 0, &probe, 1) == 1) ? 0 : -1;

        /* 再做 SMBus read-word(cmd=0x07 TOBJ1) */
        rc_obj = mlx_raw_read(bus, 0x07, buf);

        tca9548a_unlock(mux);

        rt_kprintf("  #%d addr-probe=%s  read-word=%s  raw=[%02X %02X %02X]\n",
                   i,
                   (rc_addr == 0) ? "ACK " : "NACK",
                   (rc_obj == 0) ? "OK  " : "FAIL",
                   buf[0], buf[1], buf[2]);

        rt_thread_mdelay(120);   /* MLX SMBus 连续读需要间隔 */
    }
}
MSH_CMD_EXPORT(mlx_diag, MLX90614 raw SMBus read diagnostic (prints raw bytes));
