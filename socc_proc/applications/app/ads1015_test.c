/*
 * ADS1015 ECG 通道读数自测（MSH 命令，按需手动执行）
 *
 * 经 mux CH4 选通后连续读 20 次 AIN0（AD8232 输出）。
 * 静止基线应在 AD8232 REFOUT 偏置点附近（约 1.5V 上下），贴上电极后应能
 * 看到读数随心跳起伏；此命令只读原始码，不做心率解算。
 */

#include <rtthread.h>
#include "../sensor/tca9548a.h"
#include "../sensor/ads1015.h"

#define ADS1015_MUX_CHANNEL 4  /* 拓展板设计：ADS1015 挂在 mux CH4 */

static void ads1015_selftest(void)
{
    tca9548a_device_t mux;
    ads1015_device_t adc;
    int i;

    /*
     * mux 是 ppg_task 也在持续使用的共享硬件资源（每 10ms 轮询切 CH0/CH1），
     * 这里必须拿同一个单例句柄 + lock/unlock 包住"选通道+访问下游设备"，
     * 否则两边各自独立开锁，通道会被对方随时改掉（这正是之前 read failed 的根因）。
     */
    mux = tca9548a_shared();
    if (mux == RT_NULL)
    {
        rt_kprintf("TCA9548A shared mux unavailable, abort ADS1015 test\n");
        return;
    }

    tca9548a_lock(mux);
    tca9548a_select_channel(mux, ADS1015_MUX_CHANNEL);
    adc = ads1015_init("i2c1");
    tca9548a_unlock(mux);

    if (adc == RT_NULL)
    {
        rt_kprintf("ADS1015 init failed, check CH%d wiring\n", ADS1015_MUX_CHANNEL);
        return;
    }

    rt_kprintf("AIN0 (AD8232 ECG OUTPUT) raw code, 1 code ~= 1mV @ +-2.048V range:\n");
    for (i = 0; i < 20; i++)
    {
        rt_int16_t raw = 0;
        rt_err_t err;

        tca9548a_lock(mux);
        tca9548a_select_channel(mux, ADS1015_MUX_CHANNEL);
        err = ads1015_read_single_ended(adc, 0, &raw);
        tca9548a_unlock(mux);

        if (err == RT_EOK)
        {
            rt_kprintf("  [%2d] raw=%5d  ~%dmV\n", i, raw, raw);
        }
        else
        {
            rt_kprintf("  [%2d] read failed\n", i);
        }
        rt_thread_mdelay(100);  /* 锁外延时，让 ppg_task 有机会用回 CH0/CH1 */
    }

    ads1015_deinit(adc);
    /* 不 deinit 共享 mux —— ppg_task 等其他使用者还依赖它存活 */
}
MSH_CMD_EXPORT(ads1015_selftest, ADS1015 AIN0 ECG raw reading self-test via mux CH4);

/*
 * Grove GSR 通道读数自测：同样经 mux CH4 选通 ADS1015，读 AIN1。
 * GSR 是皮肤电导测量，静止基线应比较平稳，手指用力捏一下应该能看到明显变化。
 */
static void gsr_selftest(void)
{
    tca9548a_device_t mux;
    ads1015_device_t adc;
    int i;

    /* 同 ads1015_selftest：必须用共享单例 + lock/unlock，理由见那边的注释 */
    mux = tca9548a_shared();
    if (mux == RT_NULL)
    {
        rt_kprintf("TCA9548A shared mux unavailable, abort GSR test\n");
        return;
    }

    tca9548a_lock(mux);
    tca9548a_select_channel(mux, ADS1015_MUX_CHANNEL);
    adc = ads1015_init("i2c1");
    tca9548a_unlock(mux);

    if (adc == RT_NULL)
    {
        rt_kprintf("ADS1015 init failed, check CH%d wiring\n", ADS1015_MUX_CHANNEL);
        return;
    }

    rt_kprintf("AIN1 (Grove GSR) raw code, 1 code ~= 1mV @ +-2.048V range:\n");
    for (i = 0; i < 20; i++)
    {
        rt_int16_t raw = 0;
        rt_err_t err;

        tca9548a_lock(mux);
        tca9548a_select_channel(mux, ADS1015_MUX_CHANNEL);
        err = ads1015_read_single_ended(adc, 1, &raw);
        tca9548a_unlock(mux);

        if (err == RT_EOK)
        {
            rt_kprintf("  [%2d] raw=%5d  ~%dmV\n", i, raw, raw);
        }
        else
        {
            rt_kprintf("  [%2d] read failed\n", i);
        }
        rt_thread_mdelay(100);  /* 锁外延时，让 ppg_task 有机会用回 CH0/CH1 */
    }

    ads1015_deinit(adc);
    /* 不 deinit 共享 mux —— ppg_task 等其他使用者还依赖它存活 */
}
MSH_CMD_EXPORT(gsr_selftest, ADS1015 AIN1 GSR raw reading self-test via mux CH4);
