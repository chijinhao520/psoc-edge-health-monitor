#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#define LED_PIN_B                 GET_PIN(16, 5)

int main(void)
{
    rt_kprintf("Hello RT-Thread\r\n");
    rt_kprintf("This core is cortex-m33\n");
    rt_pin_mode(LED_PIN_B, PIN_MODE_OUTPUT);
    while (1)
    {
        rt_pin_write(LED_PIN_B, PIN_HIGH);
        rt_thread_mdelay(500);
        rt_pin_write(LED_PIN_B, PIN_LOW);
        rt_thread_mdelay(500);
    }
    return 0;
}

/* ========== I2C 总线扫描调试命令 ==========
 *
 * ⚠⚠ 别扫 i2c0 —— 它是**双核共享**的物理外设，M55 的语音播报正靠它工作。
 *
 * 2026-07-17 对抗审(peripheral lens)查实：
 *   - i2c0 = SCB0，两个工程的 cycfg 里都是 CYBSP_I2C_CONTROLLER_HW=SCB0、
 *     引脚都是 P8_0/P8_1、SCB0_BASE 都是 0x42990000 → **同一块物理外设**
 *   - M33 本核 drv_i2c.c:168 的 rt_hw_i2c_init() 开机就无条件
 *     Cy_SCB_I2C_Init(SCB0) + Cy_SCB_I2C_Enable(SCB0)
 *   - M55 侧 drv_i2s.c:405 用 i2c0 跟 ES8388 codec 通信（语音播报时才跑）
 * 这违反 CLAUDE.md「同一对 GPIO 不能被两个核同时驱动」，目前不炸只是因为
 * **时序恰好错开**：M33 应用层所有传感器都走 i2c1，从不真正对 i2c0 发起传输。
 *
 * 而本命令的默认参数原本就是 "i2c0" —— 光敲一个 `i2c_scan` 就会去扫它，
 * 若此时 M55 正在播报，就是从另一个核对同一块 SCB0 发传输，打断 ES8388 会话。
 * 故默认值改为 "i2c1"（传感器实际所在的总线，本就是日常要扫的那条）。
 *
 * 真要扫 i2c0 排查 codec，请先确认 M55 没在播报。
 */
static void i2c_scan(int argc, char *argv[])
{
    /* 默认 i2c1：传感器(AHT20/mux/MAX30102/MLX/ADS1015)全在这条总线上。
     * 原默认 "i2c0" 会误撞 M55 的 ES8388，见上方警告。 */
    const char *name = (argc > 1) ? argv[1] : "i2c1";

    if (rt_strcmp(name, "i2c0") == 0)
    {
        rt_kprintf("[warn] i2c0(SCB0) 是双核共享外设，M55 语音播报正用它跟 ES8388 通信。\n");
        rt_kprintf("[warn] 扫描会从本核重新驱动 SCB0，可能打断对方的 I2C 会话。\n");
        rt_kprintf("[warn] 确认 M55 未在播报后再继续。传感器请扫 i2c1。\n");
    }
    struct rt_i2c_bus_device *bus;
    rt_uint8_t buf = 0;
    int count = 0;

    bus = rt_i2c_bus_device_find(name);
    if (bus == RT_NULL)
    {
        rt_kprintf("%s not found!\n", name);
        return;
    }

    rt_kprintf("Scanning %s ...\n", name);
    for (int addr = 0x08; addr < 0x78; addr++)
    {
        if (rt_i2c_master_recv(bus, addr, 0, &buf, 1) == 1)
        {
            rt_kprintf("  [0x%02X] ACK\n", addr);
            count++;
        }
    }
    rt_kprintf("Done. %d device(s).\n", count);
}
MSH_CMD_EXPORT(i2c_scan, scan i2c bus);
