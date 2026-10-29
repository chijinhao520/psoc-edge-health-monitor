/*
 * pat_calc.c — PAT 配对计算实现
 */

#include "pat_calc.h"
#include "../app/sensor_data.h"

#define DBG_TAG "pat_calc"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static rt_tick_t  s_pending_r_tick;
static int        s_pending_r_active;
static rt_mutex_t s_lock = RT_NULL;

int pat_calc_init(void)
{
    s_pending_r_active = 0;

    s_lock = rt_mutex_create("pat_lk", RT_IPC_FLAG_PRIO);
    if (s_lock == RT_NULL)
    {
        LOG_E("pat_calc mutex create failed");
        return -RT_ERROR;
    }
    return RT_EOK;
}
INIT_ENV_EXPORT(pat_calc_init);

void pat_on_r_peak(rt_tick_t tick)
{
    if (s_lock == RT_NULL) return;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_pending_r_tick   = tick;
    s_pending_r_active = 1;
    rt_mutex_release(s_lock);
}

void pat_on_ppg_peak(rt_tick_t tick)
{
    rt_tick_t r_tick;

    if (s_lock == RT_NULL) return;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    if (!s_pending_r_active)
    {
        rt_mutex_release(s_lock);
        return;
    }
    r_tick = s_pending_r_tick;
    /* 不管这次配不配得上，这个 R 峰只用一次——防止后面的 PPG 峰重复配对
     * 同一个 R 峰 */
    s_pending_r_active = 0;
    rt_mutex_release(s_lock);

    /* rt_tick_t 是无符号类型，PPG 峰理应晚于 R 峰；tick < r_tick 说明
     * 时序错乱（比如 R 峰其实来自上一拍残留），直接丢弃 */
    if (tick < r_tick)
        return;

    rt_uint32_t raw_ms = (rt_uint32_t)(tick - r_tick) * (1000 / RT_TICK_PER_SECOND);
    if (raw_ms < PAT_RAW_MIN_MS || raw_ms > PAT_RAW_MAX_MS)
        return;   /* 窗口外，不强配 */

    float pat_ms = (float)raw_ms - PAT_DETECT_LATENCY_MS;
    int   valid  = (pat_ms > PAT_VALID_MIN_MS && pat_ms < PAT_VALID_MAX_MS) ? 1 : 0;

    sensor_data_update_pat(valid ? pat_ms : 0.0f, (rt_bool_t)valid);
}
