/* M33 scalar publication: one owner, bounded freshness, no touch writes. */
#include "sensor_data.h"
#include "ipc_shared_data.h"
#include <math.h>

static struct sensor_data g_sensor_data;
static rt_mutex_t g_lock;
static rt_tick_t sht_tick, mlx_tick, aht_tick, gsr_tick;

static void publish_locked(void)
{
    struct sensor_data *s = &g_sensor_data;
    volatile sensor_ipc_data_t *p = ipc_get_data_ptr();
    rt_tick_t now = rt_tick_get();
    if (now - sht_tick > rt_tick_from_millisecond(3000)) s->sht30_valid = RT_FALSE;
    if (now - mlx_tick > rt_tick_from_millisecond(3000)) s->mlx_valid = RT_FALSE;
    if (now - aht_tick > rt_tick_from_millisecond(3000)) s->aht20_valid = RT_FALSE;
    if (now - gsr_tick > rt_tick_from_millisecond(1000)) s->gsr_valid = RT_FALSE;
    if (now - s->ppg_tick > rt_tick_from_millisecond(60000)) s->ppg_valid = RT_FALSE;
    /* Thermal BMR and FIFO-read-time PAT have no validated measurement contract. */
    s->bmr_valid = s->pat_valid = RT_FALSE;
    p->sequence++;
    __asm__ volatile("dsb sy" ::: "memory");
    p->heart_rate=s->heart_rate; p->spo2=s->spo2; p->blood_flow=0;
    p->ppg_valid=s->ppg_valid;
    p->t_ir=s->t_ir; p->mlx_valid=s->mlx_valid;
    p->sht30_temp=s->sht30_temp; p->sht30_humi=s->sht30_humi; p->sht30_valid=s->sht30_valid;
    p->t_board=s->t_board; p->h_board=s->h_board; p->aht_valid=s->aht20_valid;
    p->bmr=0; p->bmr_valid=0; p->glucose=0; p->glucose_valid=0;
    p->gsr_stress_index=s->gsr_stress_index; p->gsr_valid=s->gsr_valid;
    p->pat_ms=0; p->pat_valid=0;
    p->ppg_session_state=s->ppg_session_state;
    p->ppg_session_progress=s->ppg_session_progress;
    p->ppg_reject_reason=s->ppg_reject_reason;
    p->ppg_measurement_id=s->ppg_measurement_id; p->ppg_tick=s->ppg_tick;
    p->ppg_request_seq=s->ppg_request_seq;
    p->tick=now; p->magic=IPC_MAGIC;
    __asm__ volatile("dsb sy" ::: "memory");
    p->sequence++;
    __asm__ volatile("dsb sy" ::: "memory");
}

int sensor_data_init(void)
{
    rt_memset(&g_sensor_data,0,sizeof(g_sensor_data));
    rt_memset((void *)IPC_USER_DATA_ADDR,0,sizeof(sensor_ipc_data_t));
    rt_memset((void *)IPC_PPG_WAVEFORM_ADDR,0,sizeof(ppg_waveform_ipc_t));
    rt_memset((void *)IPC_ECG_WAVEFORM_ADDR,0,sizeof(ecg_waveform_ipc_t));
    __asm__ volatile("dsb sy" ::: "memory");
    g_lock=rt_mutex_create("sdata_lk",RT_IPC_FLAG_PRIO);
    return g_lock ? RT_EOK : -RT_ERROR;
}
INIT_ENV_EXPORT(sensor_data_init);

void sensor_data_refresh(void)
{
    if (!g_lock) return;
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    publish_locked();
    rt_mutex_release(g_lock);
}
void sensor_data_update_session(int state,int progress,int reject)
{
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    g_sensor_data.ppg_session_state=state;
    g_sensor_data.ppg_session_progress=progress;
    g_sensor_data.ppg_reject_reason=reject;
    publish_locked(); rt_mutex_release(g_lock);
}
void sensor_data_begin_measurement(uint32_t request_seq)
{
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    g_sensor_data.ppg_valid=RT_FALSE;
    g_sensor_data.heart_rate=g_sensor_data.spo2=0;
    g_sensor_data.ppg_request_seq=request_seq;
    publish_locked();
    rt_mutex_release(g_lock);
}
void sensor_data_update_ppg(int32_t hr,int32_t spo2,float bv,rt_bool_t valid)
{
    RT_UNUSED(bv);
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    g_sensor_data.heart_rate=hr; g_sensor_data.spo2=spo2;
    g_sensor_data.blood_flow=0; g_sensor_data.ppg_valid=valid;
    if (valid) { g_sensor_data.ppg_measurement_id++; g_sensor_data.ppg_tick=rt_tick_get(); }
    publish_locked(); rt_mutex_release(g_lock);
}
void sensor_data_update_sht30(float t,float h)
{
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    g_sensor_data.sht30_temp=t; g_sensor_data.sht30_humi=h;
    g_sensor_data.sht30_valid=isfinite(t)&&isfinite(h)&&t>=-40&&t<=125&&h>=0&&h<=100;
    sht_tick=rt_tick_get(); publish_locked(); rt_mutex_release(g_lock);
}
void sensor_data_update_mlx90614(float t)
{
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    g_sensor_data.t_ir=t; g_sensor_data.mlx_valid=isfinite(t)&&t>=-40&&t<=125;
    mlx_tick=rt_tick_get(); publish_locked(); rt_mutex_release(g_lock);
}
void sensor_data_update_aht20(float t,float h)
{
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    g_sensor_data.t_board=t; g_sensor_data.h_board=h;
    g_sensor_data.aht20_valid=isfinite(t)&&isfinite(h)&&t>=-40&&t<=85&&h>=0&&h<=100;
    aht_tick=rt_tick_get(); publish_locked(); rt_mutex_release(g_lock);
}
void sensor_data_update_bmr(float bmr)
{
    RT_UNUSED(bmr); /* No demographic profile/validated BMR source exists yet. */
    sensor_data_refresh();
}
void sensor_data_update_gsr(float raw,float stress,rt_bool_t valid)
{
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    g_sensor_data.gsr_raw_mv=raw; g_sensor_data.gsr_stress_index=stress;
    g_sensor_data.gsr_valid=valid&&isfinite(raw)&&isfinite(stress)&&stress>=0&&stress<=100;
    gsr_tick=rt_tick_get(); publish_locked(); rt_mutex_release(g_lock);
}
void sensor_data_update_pat(float pat,rt_bool_t valid)
{
    RT_UNUSED(pat); RT_UNUSED(valid);
    sensor_data_refresh();
}
void sensor_data_get(struct sensor_data *out)
{
    if (!out) return;
    rt_mutex_take(g_lock,RT_WAITING_FOREVER);
    publish_locked(); *out=g_sensor_data;
    rt_mutex_release(g_lock);
}
