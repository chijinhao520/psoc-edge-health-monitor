/*
 * ECG 连续采集任务
 *
 * 走 mux CH4 -> ADS1015 AIN0（AD8232 输出），跟 PPG（CH0/CH1）共用同一条
 * i2c1 总线和共享 mux 单例锁（tca9548a_shared）。写入 ecg_waveform_ipc_t
 * 环形缓冲，供 M55 心律不齐 NPU 任务连续消费（不是像 PPG 那样整块覆盖）。
 *
 * 采样节奏：不额外 rt_thread_mdelay，让 ads1015_read_single_ended 内部的
 * 转换等待（~1ms 量级）自然节流，实际达到的速率由 sample_rate_hz 字段实测
 * 上报，M55 根据 sample_rate_hz 重设重采样器，速率变化时清空旧窗口。
 */

#include <rtthread.h>
#include "../sensor/tca9548a.h"
#include "../sensor/ads1015.h"
#include "ipc_shared_data.h"
#include "ecg_task.h"

#define DBG_TAG "ecg_task"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define ECG_MUX_CHANNEL            4
#define ECG_RATE_REPORT_SAMPLES    500  /* 每攒够这么多样本上报一次实测速率 */

/* 兼容旧 PAT 接口；共享 I2C 的读取时刻不能代表 R 峰时刻，保持不可用。 */
static rt_tick_t         s_last_rpeak_tick = 0;
static volatile uint32_t s_rpeak_seq       = 0;

rt_tick_t ecg_task_last_rpeak_tick(void)
{
    return s_last_rpeak_tick;
}

uint32_t ecg_task_rpeak_seq(void)
{
    return s_rpeak_seq;
}

static void ecg_thread_entry(void *param)
{
    RT_UNUSED(param);

    tca9548a_device_t mux = RT_NULL;
    ads1015_device_t adc = RT_NULL;
    while (!adc) {
        if (!mux) mux=tca9548a_shared();
        if (mux && tca9548a_lock(mux)==RT_EOK) {
            if (tca9548a_select_channel(mux, ECG_MUX_CHANNEL)==RT_EOK) adc=ads1015_init("i2c1");
            tca9548a_unlock(mux);
        }
        if (!adc) rt_thread_mdelay(2000);
    }

    LOG_I("ECG sampling started");

    volatile ecg_waveform_ipc_t *ipc = ipc_get_ecg_ptr();
    rt_uint32_t sample_count = 0;
    rt_tick_t   t_window_start = rt_tick_get();

    rt_tick_t last_sample=0;
    unsigned failures=0;
    while (1)
    {
        rt_int16_t raw = 0;
        rt_err_t err;

        err=tca9548a_lock(mux);
        if (err==RT_EOK) {
            err=tca9548a_select_channel(mux, ECG_MUX_CHANNEL);
            if (err==RT_EOK) err = ads1015_read_single_ended(adc, 0, &raw);
            tca9548a_unlock(mux);
        }

        if (err != RT_EOK)
        {
            ipc->magic=0; ipc->sample_rate_hz=0; ipc->stream_epoch++;
            __asm__ volatile("dsb sy" ::: "memory");
            sample_count=0; t_window_start=rt_tick_get(); last_sample=0;
            if (!failures) LOG_W("ECG read failed: reset stream continuity");
            if (failures<10) failures++;
            rt_thread_mdelay(failures>=10 ? 1000 : 5);
            continue;
        }
        if (failures) { LOG_I("ECG connection recovered, rebuilding window"); failures=0; }

        rt_tick_t sample_tick=rt_tick_get();
        if (last_sample && sample_tick-last_sample>rt_tick_from_millisecond(20)) {
            ipc->magic=0;
            __asm__ volatile("dsb sy" ::: "memory");
            ipc->stream_epoch++; ipc->sample_rate_hz=0;
            sample_count=0; t_window_start=sample_tick;
        }
        last_sample=sample_tick;
        rt_uint32_t idx = ipc->write_seq % ECG_IPC_RING_LEN;
        ipc->ring[idx] = (float)raw;   /* mV，ADS1015 原始码在 +-2.048V 量程下约等于 mV */
        sample_count++;
        if (sample_count >= ECG_RATE_REPORT_SAMPLES)
        {
            rt_tick_t now = rt_tick_get();
            rt_uint32_t elapsed_ms = (rt_uint32_t)((uint64_t)(now - t_window_start) * 1000 / RT_TICK_PER_SECOND);
            rt_uint32_t hz = elapsed_ms ? (sample_count * 1000 / elapsed_ms) : 0;

            if (ipc->sample_rate_hz==0 ||
                (hz>ipc->sample_rate_hz ? hz-ipc->sample_rate_hz : ipc->sample_rate_hz-hz)*100 > ipc->sample_rate_hz*5) {
                ipc->magic=0;
                __asm__ volatile("dsb sy" ::: "memory");
                ipc->sample_rate_hz=hz; ipc->stream_epoch++;
            }
            LOG_I("measured rate: %u Hz (%u samples in %u ms)", hz, sample_count, elapsed_ms);

            sample_count   = 0;
            t_window_start = now;
        }
        ipc->tick=sample_tick;
        __asm__ volatile("dsb sy" ::: "memory");
        ipc->magic=IPC_ECG_MAGIC;
        __asm__ volatile("dsb sy" ::: "memory");
        ipc->write_seq++;
    }
}

int ecg_task_init(void)
{
    rt_thread_t tid = rt_thread_create(
        "ecg",
        ecg_thread_entry,
        RT_NULL,
        2048,
        11,
        10);

    if (tid == RT_NULL)
    {
        LOG_E("ECG thread create failed");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(ecg_task_init);
