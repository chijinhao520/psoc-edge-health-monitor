"""执行生产PPG任务循环，以模拟时钟/总线验证请求、取消和恢复。

替换硬件屏障和共享地址；生产状态机、任务分支与窗口管理保持原样。
"""
import re
from cross_layer_sil import ROOT, OUT, compile_pc


def pc_ipc():
    ipc = (ROOT / "socc_proc/applications/app/ipc_shared_data.h").read_text(encoding="utf-8")
    for typ, func, address, local in (
        ("sensor_ipc_data_t", "ipc_get_data_ptr", "IPC_USER_DATA_ADDR", "scalar"),
        ("ppg_waveform_ipc_t", "ipc_get_waveform_ptr", "IPC_PPG_WAVEFORM_ADDR", "wave"),
        ("ppg_cmd_ipc_t", "ipc_get_cmd_ptr", "IPC_CMD_ADDR", "command"),
    ):
        ipc = ipc.replace(f"static inline volatile {typ} *{func}", f"static {typ} test_{local};\nstatic inline volatile {typ} *{func}")
        ipc = ipc.replace(f"return (volatile {typ} *){address};", f"return &test_{local};")
    return ipc


def test_ppg_task():
    app = ROOT / "socc_proc/applications/app"
    algo = ROOT / "socc_proc/applications/algo"
    sensor = ROOT / "socc_proc/applications/sensor"
    stubs = ROOT / "socc_proc/tools/accuracy_sil/stubs"
    ipc = pc_ipc()
    task = (app / "ppg_task.c").read_text(encoding="utf-8").split("int ppg_task_init(void)")[0]
    task = re.sub(r'^#include[^\n]*', '', task, flags=re.M)
    task = task.replace('__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
    preamble = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>
#include "rtthread.h"
#include "rtdbg.h"
#include "MAX30102.h"
#include "tca9548a.h"
#include "ppg_process.h"
#include "ppg_sqi.h"
#include "ppg_session.h"
#include "ppg_downsample.h"
#define __attribute__(x)
#define RT_UNUSED(x) (void)(x)
#define rt_memcpy memcpy
#define rt_kprintf(...) ((void)0)
'''
    mocks = r'''
static rt_tick_t tick, end_tick;
static int scenario, event_idx, published, online, config_ok, initializations, released;
static int mux_held, channel, state, reason, lock_delay, reads;
static jmp_buf finish;
static struct tca9548a_device mux;
static struct max30102_device dev;
static const unsigned times[]={5100,6200,7000};
static void issue(int cmd) { test_command.magic=IPC_CMD_MAGIC;test_command.cmd=cmd;test_command.seq++; }
rt_tick_t rt_tick_get(void) { return tick; }
rt_tick_t rt_tick_from_millisecond(int ms) { return (rt_tick_t)ms; }
void rt_thread_mdelay(int ms) {
    assert(!mux_held);tick+=(rt_tick_t)ms;
    if (scenario<=2) {
        int n=scenario==2?3:2;
        while(event_idx<n && tick>=times[event_idx]) {
            issue(event_idx==1?PPG_CMD_CANCEL_MEASURE:PPG_CMD_START_MEASURE);event_idx++;
        }
    } else if(scenario==3) {
        if(event_idx==0 && tick>=5100) { issue(PPG_CMD_START_MEASURE);event_idx++; }
        if(event_idx==1 && tick>=16000) { config_ok=0;event_idx++; }
        if(event_idx==2 && tick>=27000) { issue(PPG_CMD_START_MEASURE);event_idx++; }
    } else if(scenario==4 && !event_idx && tick>=5100) {issue(PPG_CMD_START_MEASURE);event_idx++;}
    if(tick>=end_tick)longjmp(finish,1);
}
tca9548a_device_t tca9548a_shared(void) { return &mux; }
rt_err_t tca9548a_lock(tca9548a_device_t d) { (void)d;assert(!mux_held);tick+=lock_delay;mux_held=1;return 0; }
void tca9548a_unlock(tca9548a_device_t d) { (void)d;assert(mux_held);mux_held=0; }
rt_err_t tca9548a_select_channel(tca9548a_device_t d,rt_uint8_t ch) { (void)d;assert(mux_held);channel=ch;return 0; }
max30102_device_t max30102_try_init(const char *name) {
    (void)name;assert(mux_held);if(channel || !online)return NULL;
    initializations++;config_ok=1;return &dev;
}
void max30102_deinit(max30102_device_t d) { assert(d==&dev);released++; }
rt_err_t max30102_check_config(max30102_device_t d) { assert(d==&dev && mux_held && !channel);return config_ok?0:-RT_ERROR; }
rt_err_t max30102_flush_fifo(max30102_device_t d) { assert(d==&dev && mux_held && !channel);return 0; }
rt_err_t max30102_read_batch(max30102_device_t d,rt_uint32_t *red,rt_uint32_t *ir,rt_uint16_t cap,rt_uint16_t *n) {
    reads++;assert(d==&dev && mux_held && !channel);*n=config_ok?(cap<2?cap:2):0;
    for(int i=0;i<*n;i++) {red[i]=50000;ir[i]=40000;} return 0;
}
void sensor_data_update_ppg(int32_t hr,int32_t spo2,float bv,rt_bool_t valid) {
    (void)bv;test_scalar.ppg_valid=valid;test_scalar.heart_rate=hr;test_scalar.spo2=spo2;
    if(valid) { published++;test_scalar.ppg_measurement_id++; if(scenario==3)assert(tick>=27000); }
}
void sensor_data_begin_measurement(uint32_t seq) {test_scalar.ppg_valid=0;test_scalar.ppg_request_seq=seq;}
void sensor_data_update_session(int s,int progress,int why) {state=s;reason=why;(void)progress;}
rt_err_t ppg_sqi_compute(const uint32_t *ir,int32_t n,uint32_t fs,ppg_sqi_t *s) {
    (void)ir;(void)n;(void)fs;memset(s,0,sizeof(*s));s->dc=40000;s->score=100;return 0;
}
ppg_gate_result_t ppg_sqi_gate(const ppg_sqi_t *s,int32_t h,int32_t o,int8_t hv,int8_t ov) {
    (void)s;(void)h;(void)o;(void)hv;(void)ov;return PPG_GATE_PASS;
}
void maxim_heart_rate_and_oxygen_saturation(uint32_t *ir,int32_t n,uint32_t *red,int32_t *sp,int8_t *sv,int32_t *hr,int8_t *hv,uint32_t fs) {
    (void)ir;(void)n;(void)red;(void)fs;*sp=98;*sv=1;*hr=75;*hv=1;
    if(scenario==4 && tick>30000 && test_command.cmd!=PPG_CMD_CANCEL_MEASURE)issue(PPG_CMD_CANCEL_MEASURE);
}
void ppg_downsample(const uint32_t *r,const uint32_t *ir,int len,float out[PPG_DS_LEN][PPG_DS_CHANNELS]) {
    (void)r;(void)ir;(void)len;memset(out,0,sizeof(float)*PPG_DS_LEN*PPG_DS_CHANNELS);
}
'''
    tests = r'''
int main(void) {
    /* FIFO连续性必须包含等总线的时间；200ms边界及tick回绕。 */
    uint32_t red[BUFFER_SIZE], ir[BUFFER_SIZE];
    rt_uint16_t used=0;
    rt_tick_t last=100;
    tick=100;lock_delay=201;config_ok=1;
    assert(collect_channel(&mux,&dev,0,red,ir,&used,&last)==-RT_EFULL);
    assert(used==0 && reads==0 && last==301 && !mux_held);
    tick=UINT32_MAX-99;last=tick;lock_delay=200;
    assert(collect_channel(&mux,&dev,0,red,ir,&used,&last)==RT_EOK);
    assert(used==2 && reads==1 && last==100 && !mux_held);
    lock_delay=0;
    puts("PPG bus wait: deadline and tick wrap passed");
    for(scenario=0;scenario<5;scenario++) {
        tick=0;online=scenario!=0;event_idx=published=initializations=released=mux_held=0;
        state=reason=0;config_ok=1;
        memset(&test_command,0,sizeof(test_command));memset(&test_scalar,0,sizeof(test_scalar));
        memset(&test_wave,0,sizeof(test_wave));
        end_tick=scenario<=1?6900:(scenario==2?33500:(scenario==3?53500:33500));
        if(setjmp(finish)==0)ppg_thread_entry(NULL);
        assert(!mux_held);
        assert(test_scalar.ppg_request_seq==test_command.seq);
        if(scenario==0 || scenario==1 || scenario==4) {
            assert(published==0 && !test_scalar.ppg_valid && state==PPG_SESSION_CANCELLED);
        } else assert(published==1 && test_scalar.ppg_valid);
        if(scenario==3)assert(initializations>=2 && released>=1);
        printf("PPG task scenario %d: ack=%u state=%d published=%d reinit=%d passed\n",scenario,test_scalar.ppg_request_seq,state,published,initializations);
    }
    return 0;
}
'''
    print(compile_pc("device_system_sil", preamble + ipc + mocks + task + tests,
                     includes=(stubs, algo, sensor), sources=(algo / "ppg_session.c",)).strip())


def test_ui_commands():
    ui = ROOT / "Edgi_Talk_M55_LVGL/applications/ui"
    hdr = (ui / "ui_data.h").read_text(encoding="utf-8").replace('#include "lvgl.h"', '')
    src = (ui / "ui_data.c").read_text(encoding="utf-8")
    src = re.sub(r'^#include[^\n]*', '', src, flags=re.M)
    src = src.replace('__asm__ volatile("dsb sy" ::: "memory");', '(void)0;')
    preamble = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "rtthread.h"
#include "glucose_task.h"
#include "arrhythmia_task.h"
#include "health_metrics.h"
#include "health_score.h"
#define __attribute__(x)
typedef int lv_timer_t;
typedef int rt_base_t;
static rt_tick_t tick;
rt_tick_t rt_tick_get(void) { return tick; }
rt_tick_t rt_tick_from_millisecond(int ms) { return (rt_tick_t)ms; }
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t l) { (void)l; }
static void lv_timer_create(void (*cb)(lv_timer_t *),int ms,void *arg) { (void)cb;(void)ms;(void)arg; }
static uint32_t lv_tick_get(void) { return tick; }
uint32_t arrhythmia_request_session(void) { static uint32_t id;return ++id; }
void glucose_copy_result(glucose_result_t *r) { memset(r,0,sizeof(*r)); }
void arrhythmia_copy_result(arr_result_t *r) { memset(r,0,sizeof(*r)); }
void health_metrics_copy_result(health_metrics_result_t *r) { memset(r,0,sizeof(*r)); }
'''
    mock = r'''
static int ipc_sensor_snapshot(sensor_ipc_data_t *out) { *out=test_scalar; return 1; }
'''
    tests = r'''
static void poll(uint32_t time) { tick=time;ipc_poll_cb(NULL); }
int main(void) {
    test_scalar.sequence=2;test_scalar.ppg_valid=1;test_scalar.heart_rate=75;
    test_scalar.spo2=98;test_scalar.ppg_session_state=UI_MEAS_DONE;test_scalar.ppg_measurement_id=1;
    poll(100);assert(s_data.hr_valid && s_data.sensor_link_valid);
    ui_data_send_start_measure();assert(test_command.cmd==PPG_CMD_START_MEASURE && test_command.seq==1);
    test_scalar.sequence=4;poll(200);assert(!s_data.hr_valid && s_data.ppg_session_state==UI_MEAS_WAIT_FINGER);
    test_scalar.sequence=6;poll(10200);assert(!s_data.hr_valid && s_data.ppg_session_state==UI_MEAS_FAILED && s_data.ppg_reject_reason==UI_REJ_REQUEST);
    ui_data_send_cancel_measure();assert(test_command.cmd==PPG_CMD_CANCEL_MEASURE && test_command.seq==2);
    test_scalar.sequence=8;test_scalar.ppg_request_seq=1;test_scalar.ppg_valid=0;
    test_scalar.ppg_session_state=UI_MEAS_WAIT_FINGER;poll(10300);
    assert(s_data.ppg_session_state==UI_MEAS_CANCELLING && !s_data.hr_valid);
    test_scalar.sequence=10;test_scalar.ppg_request_seq=2;test_scalar.ppg_session_state=UI_MEAS_CANCELLED;
    poll(10400);assert(!s_pending_cmd && s_data.ppg_session_state==UI_MEAS_CANCELLED);
    ui_data_send_start_measure();test_scalar.sequence=12;test_scalar.ppg_request_seq=3;
    test_scalar.ppg_session_state=UI_MEAS_MEASURING;poll(10500);
    assert(!s_pending_cmd && s_data.ppg_session_state==UI_MEAS_MEASURING);
    test_scalar.sequence=14;test_scalar.ppg_valid=1;test_scalar.ppg_session_state=UI_MEAS_DONE;
    test_scalar.heart_rate=82;test_scalar.ppg_measurement_id=2;poll(10600);
    assert(s_data.hr_valid && s_data.heart_rate==82);
    poll(13601);assert(!s_data.hr_valid && !s_data.sensor_link_valid && s_data.ppg_reject_reason==UI_REJ_LINK);
    test_scalar.sequence=16;test_scalar.ppg_valid=0;poll(14000);
    assert(s_data.ppg_session_state==UI_MEAS_EXPIRED && s_data.sensor_link_valid);
    test_command.seq=UINT32_MAX;ui_data_send_start_measure();assert(test_command.seq==1);
    /* 引导流程调用真实UI命令API，并冻结完成的单项。 */
    s_data.ppg_measurement_id=7;s_data.sensor_link_valid=1;
    tick=15000;ui_checkup_start();
    assert(s_checkup.state==CHECKUP_PPG && test_command.cmd==PPG_CMD_START_MEASURE);
    s_data.ppg_session_state=UI_MEAS_DONE;s_data.ppg_measurement_id=8;
    s_data.hr_valid=s_data.spo2_valid=1;s_data.heart_rate=81;s_data.spo2=98;
    checkup_poll_cb(NULL);assert(s_checkup.state==CHECKUP_ECG && s_checkup.ecg_request==1);
    ui_checkup_skip();assert(s_checkup.state==CHECKUP_DONE);
    struct ui_health_data report;ui_checkup_report(&report);
    assert(report.heart_rate==81 && report.hr_valid && !report.hrv_valid && !report.health_score_valid);
    s_data.heart_rate=140;ui_checkup_report(&report);assert(report.heart_rate==81);
    ui_checkup_retry(CHECKUP_FINGER);assert(!s_checkup.report.hr_valid && test_command.cmd==PPG_CMD_START_MEASURE);
    ui_checkup_cancel();assert(s_checkup.state==CHECKUP_CANCELLED && test_command.cmd==PPG_CMD_CANCEL_MEASURE);
    ui_checkup_start();s_data.ppg_session_state=UI_MEAS_DONE;s_data.ppg_measurement_id=9;
    s_data.hr_valid=s_data.spo2_valid=1;s_data.heart_rate=81;s_data.spo2=98;
    checkup_poll_cb(NULL);
    s_data.ecg_session_id=s_checkup.ecg_request;s_data.arr_valid=s_data.arr_pvc_valid=s_data.hrv_valid=1;
    s_data.arr_beat_count=80;s_data.arr_class_count[2]=40;s_data.arr_pvc_per_min=10;s_data.hrv_sdnn_ms=10;
    checkup_poll_cb(NULL);ui_checkup_report(&report);
    assert(s_checkup.state==CHECKUP_DONE && report.health_score_valid && report.health_score==65);
    s_data.arr_pvc_per_min=0;s_data.arr_class_count[2]=0;s_data.hrv_sdnn_ms=50;
    ui_checkup_report(&report);assert(report.health_score==65);
    puts("UI command flow: old result gating, request timeout, cancel acknowledgement, retry, stale source, expired result and sequence wrap passed");
    return 0;
}
'''
    print(compile_pc("ui_command_sil", preamble + pc_ipc() + hdr + '\n#include "ui_checkup.h"\n' + mock + src + tests,
                     includes=(ROOT / "socc_proc/tools/accuracy_sil/stubs",ui),
                     sources=(ROOT / "Edgi_Talk_M55_LVGL/applications/health_score.c",)).strip())


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    test_ppg_task()
    test_ui_commands()
