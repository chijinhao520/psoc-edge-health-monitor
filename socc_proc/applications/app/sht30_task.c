/*
 * SHT30 sampling task
 *
 * 模块阶段：SHT30 外接远离板子，测真实环境温湿度（替代受热漂移影响的板载 AHT20）
 * 集成板阶段：SHT30 贴指腹，测接触温度和局部湿度
 *
 * 采样频率 ~1Hz，数据写入 sensor_data 供 BMR 计算使用。
 */

#include <rtthread.h>
#include "../sensor/sht30.h"
#include "sensor_data.h"
#include "ipc_shared_data.h"
#include <math.h>

#define DBG_TAG "sht30_task"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static void sht30_thread_entry(void *param)
{
    struct sht30_data data;

    RT_UNUSED(param);

    sht30_device_t dev = RT_NULL;
    while (!(dev=sht30_init("i2c1"))) rt_thread_mdelay(2000);
    LOG_I("SHT30 init success, start sampling");

    while (1)
    {
        if (sht30_read(dev, &data) == RT_EOK)
        {
            sensor_data_update_sht30(data.temperature, data.humidity);
            LOG_I("Temp: %.2f C | Humi: %.2f %%", data.temperature, data.humidity);


        }
        else
        {
            sensor_data_update_sht30(NAN,NAN);
            LOG_W("SHT30 read failed, check sensor connection");
        }

        rt_thread_mdelay(1000);
    }
}

int sht30_task_init(void)
{
    /*
     * 优先级 10：与 mlx90614 同级，均**高于** ppg(12)。
     *
     * 理由同 mlx90614_task.c 的详细注释：软 I2C 是 bit-bang，多字节读被
     * 抢占就会时序错乱读出 0xFF。SHT30 一次读 6 字节(温度2+CRC1+湿度2+CRC1)，
     * 比 MLX 还长，在 ppg(12) 以 2ms delay 死循环占 CPU 的情况下必然重蹈覆辙。
     * 本任务 1Hz、每次几毫秒，抢占 ppg 的代价可忽略。
     *
     * （SHT30 当前未接线，此处先按正确的优先级排布放好，避免接上后
     *   再花一轮排查同一个坑。）
     */
    rt_thread_t tid = rt_thread_create(
        "sht30",
        sht30_thread_entry,
        RT_NULL,
        1024,
        10,
        10);

    if (tid == RT_NULL)
    {
        LOG_E("SHT30 thread create failed");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(sht30_task_init);
