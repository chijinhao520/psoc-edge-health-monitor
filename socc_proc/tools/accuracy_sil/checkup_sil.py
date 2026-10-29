"""直接编译引导体检状态机：轮次隔离、重试、跳过、取消、超时与快照。"""
from cross_layer_sil import ROOT, OUT, compile_pc


def main():
    ui=ROOT / "Edgi_Talk_M55_LVGL/applications/ui"
    hdr=(ui / "ui_data.h").read_text(encoding="utf-8").replace('#include "lvgl.h"','')
    source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + hdr + r'''
#include "checkup_flow.h"
int main(void) {
    checkup_flow_t f={0};
    struct ui_health_data d={0};
    d.sensor_link_valid=1;d.ppg_measurement_id=12;
    d.hr_valid=d.spo2_valid=1;d.ppg_session_state=UI_MEAS_DONE;
    d.heart_rate=75;d.spo2=98;
    assert(checkup_start(&f,100,&d)==CHECKUP_ACT_PPG);
    assert(f.id==1 && !f.report.hr_valid && !f.completed);
    assert(checkup_update(&f,200,&d)==CHECKUP_ACT_NONE && f.state==CHECKUP_PPG);
    d.ppg_measurement_id=13;
    assert(checkup_update(&f,300,&d)==CHECKUP_ACT_ECG);
    assert(f.completed==1 && f.report.heart_rate==75);
    f.ecg_request=7;d.ecg_session_id=6;
    d.arr_valid=d.arr_pvc_valid=d.hrv_valid=1;d.arr_beat_count=80;
    d.arr_class_count[2]=10;d.arr_pvc_per_min=10;d.hrv_sdnn_ms=42;
    assert(checkup_update(&f,400,&d)==0 && f.state==CHECKUP_ECG); /* 旧心电结果 */
    d.ecg_session_id=7;d.hrv_valid=0;
    assert(checkup_update(&f,500,&d)==0 && f.state==CHECKUP_ECG); /* 缺HRV */
    d.hrv_valid=1;d.sensor_link_valid=0;
    assert(checkup_update(&f,600,&d)==0 && f.state==CHECKUP_ECG);
    d.sensor_link_valid=1;
    assert(checkup_update(&f,700,&d)==0 && f.state==CHECKUP_DONE && f.completed==3);
    assert(f.report.arr_pvc_per_min==10 && f.report.arr_class_count[2]==10);
    d.heart_rate=140;d.hrv_sdnn_ms=100;
    checkup_update(&f,900,&d);
    assert(f.report.heart_rate==75 && f.report.hrv_sdnn_ms==42); /* 报告冻结 */
    assert(checkup_select(&f,1,1000,&d)==CHECKUP_ACT_PPG);
    assert(!f.report.hr_valid && f.report.hrv_valid && f.completed==2);
    d.ppg_measurement_id=14;d.heart_rate=80;
    assert(checkup_update(&f,1100,&d)==0 && f.state==CHECKUP_DONE);
    assert(f.report.heart_rate==80 && f.report.hrv_sdnn_ms==42); /* 单项重试 */
    assert(checkup_select(&f,2,1200,&d)==CHECKUP_ACT_ECG);
    assert(f.report.hr_valid && !f.report.hrv_valid && f.completed==1);
    checkup_skip(&f,1300,&d);
    assert(f.state==CHECKUP_DONE && f.resolved==3 && f.completed==1);
    checkup_start(&f,2000,&d);
    assert(f.id==2 && !f.completed && !f.report.hr_valid && !f.report.hrv_valid);
    d.ppg_session_state=UI_MEAS_FAILED;d.ppg_reject_reason=UI_REJ_MOTION;
    checkup_update(&f,2100,&d);
    assert(f.state==CHECKUP_PPG_FAILED && f.reason==UI_REJ_MOTION);
    assert(checkup_skip(&f,2200,&d)==CHECKUP_ACT_ECG);
    assert(f.resolved==1 && f.completed==0);
    f.ecg_request=8;d.ecg_session_id=7;
    checkup_update(&f,102200,&d);
    assert(f.state==CHECKUP_ECG_FAILED);
    checkup_skip(&f,102300,&d);
    assert(f.state==CHECKUP_DONE && f.completed==0 && f.resolved==3);
    checkup_start(&f,UINT32_MAX-999,&d);d.ppg_session_state=UI_MEAS_MEASURING;
    checkup_update(&f,98999,&d);assert(f.state==CHECKUP_PPG);
    checkup_update(&f,99000,&d);assert(f.state==CHECKUP_PPG_FAILED); /* 回绕 */
    for (int state=CHECKUP_PPG;state<=CHECKUP_ECG_FAILED;state++) {
        f.state=state;checkup_cancel(&f,100000);
        assert(f.state==CHECKUP_CANCELLED);
        d.ppg_measurement_id++;d.ppg_session_state=UI_MEAS_DONE;
        checkup_update(&f,100001,&d);
        assert(f.state==CHECKUP_CANCELLED); /* 取消后的迟到结果 */
    }
    f.id=UINT32_MAX;checkup_start(&f,0,&d);assert(f.id==1);
    puts("Checkup: stale IDs, partial data, frozen report, isolated retries, skip, cancellation and wrap passed");
    return 0;
}
'''
    OUT.mkdir(parents=True,exist_ok=True)
    print(compile_pc("checkup_sil",source,includes=(ui,)))


if __name__=='__main__': main()
