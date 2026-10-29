/*
 * GSR 连续采集任务
 *
 * 走 mux CH4 -> ADS1015 AIN1（Grove GSR 输出），跟 ECG（AIN0）共用同一颗
 * ADS1015 芯片和同一条 i2c1 总线，靠 tca9548a_shared() 单例锁互斥。
 *
 * 跟 ecg_task 不同：GSR 只需要 50Hz（皮肤电导变化远慢于心电/PPG），所以
 * 主动 rt_thread_mdelay 节流，不贴着 ADS1015 转换速度跑满，给 ECG 更多总线
 * 时间。EMA 平滑 + 应激指数计算见 algo/gsr_process.c。
 */

#include <rtthread.h>
#include "../sensor/tca9548a.h"
#include "../sensor/ads1015.h"
#include "../algo/gsr_process.h"
#include "sensor_data.h"

#define DBG_TAG "gsr_task"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define GSR_MUX_CHANNEL         4  /* 拓展板设计：ADS1015 挂在 mux CH4，跟 ECG 共享 */
#define GSR_ADC_CHANNEL         1  /* AIN1 = Grove GSR */
#define GSR_SAMPLE_PERIOD_MS    20 /* 50Hz */
#define GSR_LOG_PERIOD_SAMPLES  250 /* 每约 5s 打印一次状态 */

static void gsr_thread_entry(void *param)
{
    RT_UNUSED(param);

    /*
     * mux 是 ecg_task/ppg_task 等多个使用者共享的硬件资源，必须拿同一个
     * 单例句柄 + lock/unlock 包住"选通道+访问下游设备"，理由同 ecg_task.c。
     */
    tca9548a_device_t mux = RT_NULL;
    ads1015_device_t adc = RT_NULL;
    while (!adc) {
        if (!mux) mux=tca9548a_shared();
        if (mux && tca9548a_lock(mux)==RT_EOK) {
            if (tca9548a_select_channel(mux,GSR_MUX_CHANNEL)==RT_EOK) adc=ads1015_init("i2c1");
            tca9548a_unlock(mux);
        }
        if (!adc) rt_thread_mdelay(2000);
    }

    LOG_I("GSR sampling started");

    gsr_state_t st;
    gsr_state_reset(&st);
    rt_uint32_t log_count = 0;
    rt_tick_t last_sample = 0;
    rt_bool_t have_sample = RT_FALSE;
    unsigned failures=0;

    while (1)
    {
        rt_int16_t raw = 0;
        rt_err_t err;

        err = tca9548a_lock(mux);
        if (err == RT_EOK) {
            err = tca9548a_select_channel(mux, GSR_MUX_CHANNEL);
            if (err == RT_EOK)
                err = ads1015_read_single_ended(adc, GSR_ADC_CHANNEL, &raw);
            tca9548a_unlock(mux);
        }

        if (err != RT_EOK)
        {
            gsr_state_reset(&st);
            have_sample = RT_FALSE;
            sensor_data_update_gsr(0.0f, 0.0f, RT_FALSE);
            if (!failures) LOG_W("GSR read failed, relearn baseline on recovery");
            if (failures<10) failures++;
            rt_thread_mdelay(failures>=10 ? 1000 : GSR_SAMPLE_PERIOD_MS);
            continue;
        }
        if (failures) { LOG_I("GSR connection recovered"); failures=0; }

        rt_tick_t now = rt_tick_get();
        if (have_sample && now - last_sample > rt_tick_from_millisecond(100))
            gsr_state_reset(&st);
        last_sample = now;
        have_sample = RT_TRUE;
        float stress_index = 0.0f;
        int ready = gsr_process_sample(&st, (float)raw, &stress_index);

        sensor_data_update_gsr((float)raw, stress_index, (rt_bool_t)ready);

        log_count++;
        if (log_count >= GSR_LOG_PERIOD_SAMPLES)
        {
            log_count = 0;
            if (ready)
                LOG_I("raw=%dmV ema=%.1fmV baseline=%.1fmV stress=%.0f",
                      raw, st.ema_mv, st.baseline_mv, stress_index);
            else
                LOG_I("learning baseline... (%u/%d samples)",
                      st.sample_count, GSR_LEARN_SAMPLES);
        }

        rt_thread_mdelay(GSR_SAMPLE_PERIOD_MS);
    }
}

int gsr_task_init(void)
{
    rt_thread_t tid = rt_thread_create(
        "gsr",
        gsr_thread_entry,
        RT_NULL,
        1536,
        16,
        10);

    if (tid == RT_NULL)
    {
        LOG_E("GSR thread create failed");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(gsr_task_init);
