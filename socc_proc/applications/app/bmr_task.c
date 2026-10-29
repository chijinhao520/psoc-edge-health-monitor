/* Freshness heartbeat. Thermal inputs cannot establish demographic BMR. */
#include <rtthread.h>
#include "sensor_data.h"
static void bmr_thread_entry(void *param)
{
    RT_UNUSED(param);
    while (1) { sensor_data_refresh(); rt_thread_mdelay(1000); }
}
int bmr_task_init(void)
{
    rt_thread_t t=rt_thread_create("sdata_pub",bmr_thread_entry,RT_NULL,1024,15,10);
    if (!t) return -RT_ERROR;
    rt_thread_startup(t); return RT_EOK;
}
INIT_APP_EXPORT(bmr_task_init);
