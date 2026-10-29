"""离线编译生产报文/PVC代码片段，并用 ARM 编译器核对 IPC 布局。

不导入云端服务、不联网；生成物仅写入 cache/accuracy_audit。
"""
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "cache/accuracy_audit"
M55 = ROOT / "Edgi_Talk_M55_LVGL/applications"
VCVARS = Path("C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Auxiliary/Build/vcvars64.bat")
GCC = Path("E:/RT-ThreadStudio/repo/Extract/ToolChain_Support_Packages/ARM/GNU_Tools_for_ARM_Embedded_Processors/13.3/bin/arm-none-eabi-gcc.exe")


def function(source, name):
    """提取生产函数（本组函数体无包含花括号的字符串）。"""
    start = re.search(r"^static [^\n]*\b" + name + r"\([^\n]*\)\s*\{", source, re.M)
    assert start, name
    # build_payload 含 JSON 花括号，使用下一段落边界单独提取。
    pos, depth = source.index("{", start.start()) + 1, 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start.start():pos]


def compile_pc(name, source, includes=(), sources=()):
    cfile, exe = OUT / (name + ".c"), OUT / (name + ".exe")
    cfile.write_text(source, encoding="utf-8")
    extra = " ".join(f'/I"{p}"' for p in includes) + " " + " ".join(f'"{p}"' for p in sources)
    command = (f'@echo off\ncall "{VCVARS}" >nul 2>&1\n'
               f'cl /nologo /utf-8 /W3 /I"{M55}" {extra} "{cfile}" '
               f'/Fo:"{OUT.as_posix()}/" /Fe:"{exe}"\n')
    batch = OUT / (name + ".cmd")
    batch.write_text(command, encoding="utf-8")
    subprocess.run(["cmd", "/c", str(batch)], check=True)
    return subprocess.check_output([str(exe)], text=True, encoding="utf-8")


def test_mqtt():
    src = (M55 / "mqtt_upload.c").read_text(encoding="utf-8")
    hdr = (M55 / "ui/ui_data.h").read_text(encoding="utf-8")
    struct = re.search(r"struct ui_health_data \{.*?\n\};", hdr, re.S).group()
    fmt = src[src.index("static void fmt2"):src.index("/* ------------------------------------------------------------------ */", src.index("static void fmt2"))]
    payload = src[src.index("static int build_payload"):src.index("/* ------------------------------------------------------------------ */", src.index("static int build_payload"))]
    cap = int(re.search(r"#define MQTT_PAYLOAD_LEN\s+(\d+)", src).group(1))
    c = r'''
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#define rt_snprintf snprintf
#define RT_NULL NULL
''' + struct + r'''
static struct ui_health_data fixture;
static const char s_device_id[]="0123456789abcdef";
static void ui_data_copy(struct ui_health_data *d) { *d=fixture; }
''' + fmt + payload + r'''
int main(void) {
    char b[1024];
    assert(build_payload(NULL,1024)==-1);
    assert(build_payload(b,0)==-1);
    assert(build_payload(b,-1)==-1);
    fixture.env_temp=-0.5f;
    int n=build_payload(b,sizeof(b)); assert(n>0 && n<sizeof(b)); puts(b);
    fixture=(struct ui_health_data){0};
    fixture.heart_rate=150; fixture.spo2=100; fixture.body_temp=60;
    fixture.glucose=35; fixture.bmr=500; fixture.env_temp=-40;
    fixture.env_humidity=100; fixture.blood_flow=0;
    fixture.arr_class=3; fixture.arr_source=1;
    fixture.arr_beat_count=UINT32_MAX; fixture.arr_pvc_per_min=UINT32_MAX;
    fixture.hrv_rmssd_ms=2000; fixture.resp_rate_bpm=60; fixture.health_score=100;
    fixture.hr_valid=fixture.spo2_valid=fixture.body_temp_valid=1;
    fixture.glucose_valid=fixture.bmr_valid=fixture.env_valid=1;
    fixture.arr_valid=fixture.arr_pvc_valid=fixture.hrv_valid=1;
    fixture.resp_valid=fixture.health_score_valid=1;
    n=build_payload(b,sizeof(b)); assert(n>0 && n<sizeof(b)); puts(b);
    /* 所有不足长度均拒绝，且不得写到调用方缓冲边界外。 */
    for (int cap=1;cap<=n+1;cap++) {
        unsigned char guarded[1026]; memset(guarded,0x5A,sizeof(guarded));
        int result=build_payload((char *)guarded+1,cap);
        assert(guarded[0]==0x5A && guarded[cap+1]==0x5A);
        assert(cap<=n ? result==-1 : result==n);
    }
    return 0;
}
'''
    lines = compile_pc("mqtt_sil", c).splitlines()

    def unique(pairs):
        result = {}
        for key, value in pairs:
            assert key not in result, f"duplicate key: {key}"
            result[key] = value
        return result

    docs = [json.loads(line, object_pairs_hook=unique) for line in lines]
    assert docs[0]["env_temp"] == -0.5
    flags = {"hr_valid", "spo2_valid", "body_temp_valid", "glucose_valid",
             "bmr_valid", "env_valid", "blood_flow_valid", "arr_valid",
             "arr_pvc_valid", "hrv_valid", "resp_valid", "health_score_valid"}
    assert flags <= docs[0].keys()
    assert all(docs[0][key] == 0 for key in flags)
    assert all(docs[1][key] == (key != "blood_flow_valid") for key in flags)
    assert docs[1]["arr_source"] == 1
    assert all(len(line.encode()) < cap for line in lines)
    print(f"MQTT: 2 JSON fixtures, unique keys, flags, negative temperature; max {max(map(len, lines))}/{cap} bytes; capacity guards passed")


def test_pvc():
    src = (M55 / "arrhythmia_task.c").read_text(encoding="utf-8")
    state = src[src.index("static arr_result_t s_result"):src.index("void arrhythmia_copy_result")]
    c = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "arrhythmia_task.h"
#define ARR_ECG_LEN 216
#define ARR_RR_DIM 3
#define ARR_N_CLASS 4
#define ARR_PVC_WIN_MS 60000
#define LOG_I(...) ((void)0)
#define rt_memset memset
typedef uint32_t rt_tick_t;
typedef int rt_base_t;
static rt_tick_t now_tick;
static int test_cls;
static uint32_t s_ecg_read_seq;
static rt_tick_t rt_tick_get(void) { return now_tick; }
static rt_tick_t rt_tick_from_millisecond(uint32_t ms) { return ms; }
static rt_base_t rt_hw_interrupt_disable(void) { return 0; }
static void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
static void arr_preproc_reset(void) {}
static void health_metrics_reset(void) {}
static int arrhythmia_inference_is_ready(void) { return 1; }
static int arrhythmia_inference_invoke(const float *e,const float *r,int *cls,float *conf,float *p) {
    (void)e;(void)r; *cls=test_cls; *conf=1; p[test_cls]=1; return 0;
}
''' + state + "\n".join(function(src, name) for name in
                         ("l3_fallback", "process_beat", "reset_continuity", "accept_session", "update_pvc_window")) + r'''
static void beat(uint32_t tick,int cls) { now_tick=tick;test_cls=cls;process_beat(NULL,NULL); }
int main(void) {
    update_pvc_window(60000); assert(!s_result.pvc_valid);
    beat(1000,2); beat(31000,2);
    update_pvc_window(60999); assert(!s_result.pvc_valid);
    update_pvc_window(61000); assert(s_result.pvc_valid && s_result.pvc_per_min==2);
    beat(62000,2); beat(92000,2); beat(122000,2);
    update_pvc_window(151000); assert(s_result.pvc_per_min==2); /* 3 / 90s */
    reset_continuity(); assert(!s_result.valid && !s_result.pvc_valid && !s_pvc_active);
    beat(200000,0); update_pvc_window(259999); assert(!s_result.pvc_valid);
    update_pvc_window(260000); assert(s_result.pvc_valid && s_result.pvc_per_min==0);
    reset_continuity(); beat(UINT32_MAX-29999,2);
    update_pvc_window(29999); assert(!s_result.pvc_valid);
    update_pvc_window(30000); assert(s_result.pvc_valid && s_result.pvc_per_min==1);
    uint32_t requested=arrhythmia_request_session();
    assert(requested==1 && s_result.session_id==0); /* 请求异步，不跨线程清算法 */
    accept_session(1234);
    assert(s_ecg_read_seq==1234 && s_result.session_id==requested && !s_result.valid && !s_result.pvc_valid);
    beat(40000,0);assert(s_result.session_id==requested && s_result.beat_count==1);
    reset_continuity();assert(s_result.session_id==requested && !s_result.valid);
    s_requested_session=UINT32_MAX;assert(arrhythmia_request_session()==1);
    puts("PVC: warmup, 60/90-second timing, reset, zero count and tick wrap passed");
    return 0;
}
'''
    print(compile_pc("pvc_sil", c).strip())


def test_ipc():
    m33 = ROOT / "socc_proc/applications/app/ipc_shared_data.h"
    assert m33.read_bytes() == (M55 / "ui/ipc_shared_data.h").read_bytes()
    c = OUT / "ipc_layout.c"
    c.write_text(r'''
#include "ipc_shared_data.h"
#include <stddef.h>
_Static_assert(IPC_USER_DATA_ADDR+sizeof(sensor_ipc_data_t)<=IPC_PPG_WAVEFORM_ADDR,"scalar overlap");
_Static_assert(sizeof(ppg_waveform_ipc_t)==1056,"PPG size");
_Static_assert(sizeof(ecg_waveform_ipc_t)==2080,"ECG size");
_Static_assert(sizeof(ppg_cmd_ipc_t)==32,"command size");
_Static_assert(IPC_PPG_WAVEFORM_ADDR+sizeof(ppg_waveform_ipc_t)<=IPC_ECG_WAVEFORM_ADDR,"PPG overlap");
_Static_assert(IPC_ECG_WAVEFORM_ADDR+sizeof(ecg_waveform_ipc_t)<=IPC_CMD_ADDR,"ECG overlap");
_Static_assert(_Alignof(sensor_ipc_data_t)==32 && _Alignof(ppg_waveform_ipc_t)==32 &&
               _Alignof(ecg_waveform_ipc_t)==32 && _Alignof(ppg_cmd_ipc_t)==32,"cache alignment");
_Static_assert(offsetof(ppg_waveform_ipc_t,measurement_id)==1036,"PPG id offset");
_Static_assert(offsetof(ecg_waveform_ipc_t,stream_epoch)==2064,"ECG epoch offset");
''', encoding="utf-8")
    for cpu in ("cortex-m33", "cortex-m55"):
        subprocess.run([str(GCC), "-std=c11", f"-mcpu={cpu}", "-mthumb", "-Werror",
                        "-I", str(m33.parent), "-c", str(c), "-o", str(OUT / f"ipc_{cpu}.o")], check=True)
    print("IPC: byte-identical headers; both ARM targets passed layout/alignment assertions")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    test_mqtt()
    test_pvc()
    test_ipc()
