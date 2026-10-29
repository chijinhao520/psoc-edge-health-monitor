/* 编译固件原实现，使用已知真值检查算法边界、时钟与连续性。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ppg_process.h"
#include "health_metrics.h"
#include "arrhythmia_preproc.h"
#include "gsr_process.h"
#include "ppg_session.h"

rt_tick_t rt_tick_get(void) { return 0; }
rt_tick_t rt_tick_from_millisecond(int ms) { return (rt_tick_t)ms; }

#define PI 3.14159265358979323846
static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    printf("FAIL line %d: %s\n",__LINE__,#expr); failures++; } } while (0)
static uint32_t ir[BUFFER_SIZE], red[BUFFER_SIZE];
static int32_t spo2, hr;
static int8_t sv, hv;
static void calc(int len, uint32_t fs)
{
    maxim_heart_rate_and_oxygen_saturation(ir,len,red,&spo2,&sv,&hr,&hv,fs);
}
static void ppg_sine(double ratio)
{
    for (int i=0;i<BUFFER_SIZE;i++) {
        double v=sin(2*PI*i/100.0);
        ir[i]=(uint32_t)(100000+5000*v);
        red[i]=(uint32_t)(100000+5000*ratio*v);
    }
}
static void test_ppg(void)
{
    ppg_sine(1);
    calc(BUFFER_SIZE,100);
    CHECK(hv && abs(hr-60)<=1);
    CHECK(sv && abs(spo2-80)<=1); /* R=1 -> 80.139%, 不应接近100%。 */
    ppg_sine(0.6);
    calc(BUFFER_SIZE,100);
    CHECK(hv && abs(hr-60)<=1);
    CHECK(sv && abs(spo2-97)<=2);
    for (int i=0;i<BUFFER_SIZE;i++) ir[i]=red[i]=50000;
    calc(BUFFER_SIZE,100);
    CHECK(!hv && !sv); /* 强波形之后平波，静态工作区不能泄漏峰。 */
    for (int i=0;i<BUFFER_SIZE;i++) ir[i]=red[i]=0;
    calc(BUFFER_SIZE,100); CHECK(!hv && !sv);
    for (int i=0;i<BUFFER_SIZE;i++) ir[i]=red[i]=0x3FFFF;
    calc(BUFFER_SIZE,100); CHECK(!hv && !sv);
    ir[0]=0x40000;
    calc(BUFFER_SIZE,100); CHECK(!hv && !sv && hr==-999 && spo2==-999);
    ppg_sine(1);
    const int bad_len[]={-1,0,1,499,501,10000};
    for (int i=0;i<6;i++) { calc(bad_len[i],100); CHECK(!hv && !sv); }
    calc(BUFFER_SIZE,0); CHECK(!hv && !sv);
    calc(BUFFER_SIZE,1001); CHECK(!hv && !sv);
    maxim_heart_rate_and_oxygen_saturation(NULL,BUFFER_SIZE,red,&spo2,&sv,&hr,&hv,100);
    CHECK(!hv && !sv);
    maxim_heart_rate_and_oxygen_saturation(ir,BUFFER_SIZE,NULL,&spo2,&sv,&hr,&hv,100);
    CHECK(!hv && !sv);
    maxim_heart_rate_and_oxygen_saturation(ir,BUFFER_SIZE,red,NULL,&sv,&hr,&hv,100);
    CHECK(!hv && !sv);
    int32_t flat_top[]={0,2,2,2,2}, locs[15], n=0;
    maxim_peaks_above_min_height(locs,&n,flat_top,5,1);
    CHECK(n==0); /* 尾部平顶不能读取数组外一个元素。 */
    CHECK(maxim_blood_flow_rate(ir,BUFFER_SIZE)==0);
    /* 极端但合法18位输入，返回值不得越界或留下旧输出。 */
    uint32_t seed=7;
    for (int frame=0;frame<300;frame++) {
        for (int i=0;i<BUFFER_SIZE;i++) {
            seed=1664525*seed+1013904223; ir[i]=seed&0x3FFFF;
            seed=1664525*seed+1013904223; red[i]=seed&0x3FFFF;
        }
        calc(BUFFER_SIZE,100);
        CHECK(!sv || (spo2>=0 && spo2<=100));
        CHECK(!hv || (hr>0 && hr<=6000));
    }
}
static void test_sqi(void)
{
    ppg_sqi_t q;
    ppg_sine(0.6);
    CHECK(ppg_sqi_compute(ir,BUFFER_SIZE,100,&q)==RT_EOK);
    CHECK(q.n_peaks==5 && q.score==100);
    CHECK(ppg_sqi_gate(&q,60,97,1,1)==PPG_GATE_PASS);
    /* 独立已知三角波：4点平均后峰高=0.95A，窗口均值=120000。
     * 幅度方差722e6，旧12次开方迭代在此量程尚未收敛。 */
    for (int i=0;i<BUFFER_SIZE;i++) ir[i]=100000;
    for (int beat=0;beat<5;beat++) {
        int a=60000+beat*20000, center=50+beat*100;
        for (int j=-19;j<=19;j++) ir[center+j]+=a*(20-abs(j))/20;
    }
    CHECK(ppg_sqi_compute(ir,BUFFER_SIZE,100,&q)==RT_EOK);
    CHECK(q.n_peaks==5 && q.dc==120000);
    CHECK(fabsf(q.pi-125.0f)<0.001f);
    CHECK(fabsf(q.peak_amp_cv-sqrtf(722000000.0f)/75000.0f)<0.00001f);
    CHECK(q.ibi_std_ms==0);
    CHECK(ppg_sqi_gate(&q,60,97,1,1)==PPG_GATE_REJ_MOTION);
    /* 单个宽平台不能因持续超过最小峰距而变成多次心跳。 */
    for (int i=0;i<BUFFER_SIZE;i++) ir[i]=(i>=100 && i<400)?110000:100000;
    CHECK(ppg_sqi_compute(ir,BUFFER_SIZE,100,&q)==RT_EOK);
    CHECK(q.n_peaks==1 && q.score==0);
    for (int i=0;i<BUFFER_SIZE;i++) ir[i]=(i>=100)?110000:100000;
    CHECK(ppg_sqi_compute(ir,BUFFER_SIZE,100,&q)==RT_EOK);
    CHECK(q.n_peaks==0 && q.score==0); /* 未结束的平台 */
    const uint32_t bad_fs[]={0,1001,UINT32_MAX};
    for (int i=0;i<3;i++) {
        q.dc=40000; q.score=100;
        CHECK(ppg_sqi_compute(ir,BUFFER_SIZE,bad_fs[i],&q)==-RT_EINVAL);
        CHECK(q.dc==0 && q.score==0);
    }
    CHECK(ppg_sqi_compute(NULL,BUFFER_SIZE,100,&q)==-RT_EINVAL);
    CHECK(q.score==0);
    CHECK(ppg_sqi_compute(ir,BUFFER_SIZE+1,100,&q)==-RT_EINVAL);
    CHECK(ppg_sqi_compute(ir,5,100,&q)==-RT_EINVAL);
    CHECK(ppg_sqi_compute(ir,BUFFER_SIZE,100,NULL)==-RT_EINVAL);
    const uint32_t bad_adc[]={0,0x3FFFF,0x40000,UINT32_MAX};
    for (int i=0;i<4;i++) {
        ppg_sine(0.6); ir[250]=bad_adc[i]; q.score=100;
        CHECK(ppg_sqi_compute(ir,BUFFER_SIZE,100,&q)==-RT_EINVAL);
        CHECK(q.score==0 && q.dc==0);
    }
    ppg_sine(0.6);
    CHECK(ppg_sqi_compute(ir,BUFFER_SIZE,100,&q)==RT_EOK && q.score==100);
    ppg_sqi_t good=q;
    const float bad_stat[]={NAN,INFINITY,-INFINITY,-1};
    for (int i=0;i<4;i++) {
        q=good; q.pi=bad_stat[i]; CHECK(ppg_sqi_gate(&q,60,97,1,1)!=PPG_GATE_PASS);
        q=good; q.peak_amp_cv=bad_stat[i]; CHECK(ppg_sqi_gate(&q,60,97,1,1)!=PPG_GATE_PASS);
        q=good; q.ibi_std_ms=bad_stat[i]; CHECK(ppg_sqi_gate(&q,60,97,1,1)!=PPG_GATE_PASS);
    }
    q=good; q.n_peaks=0; CHECK(ppg_sqi_gate(&q,60,97,1,1)!=PPG_GATE_PASS);
    q=good; q.score=101; CHECK(ppg_sqi_gate(&q,60,97,1,1)!=PPG_GATE_PASS);
}
static health_metrics_result_t hm(void)
{
    health_metrics_result_t r;
    health_metrics_copy_result(&r);
    return r;
}
static void test_hrv(void)
{
    health_metrics_reset();
    for (int i=0;i<29;i++) health_metrics_feed_rr(1000);
    CHECK(!hm().hrv_valid);
    health_metrics_feed_rr(1000);
    CHECK(hm().hrv_valid && hm().hrv_rmssd_ms==0 && hm().hrv_sdnn_ms==0);
    CHECK(!hm().resp_valid);
    const float invalid[]={NAN,INFINITY,-INFINITY,0,299,2001};
    for (int k=0;k<6;k++) {
        health_metrics_feed_rr(invalid[k]); CHECK(!hm().hrv_valid && !hm().resp_valid);
        for (int i=0;i<40;i++) health_metrics_feed_rr(1000);
        CHECK(hm().hrv_valid);
    }
    health_metrics_reset();
    for (int i=0;i<40;i++) health_metrics_feed_rr(1000+(i%2 ? 20.0f : -20.0f));
    CHECK(hm().hrv_valid);
    CHECK(fabsf(hm().hrv_rmssd_ms-40.0f)<0.01f);
    CHECK(fabsf(hm().hrv_sdnn_ms-sqrtf(16000.0f/39))<0.01f);
    health_metrics_reset();
    for (int i=0;i<40;i++) health_metrics_feed_rr(800);
    CHECK(hm().hrv_valid);
    health_metrics_feed_rr(600); CHECK(!hm().hrv_valid);
    for (int i=0;i<60;i++) health_metrics_feed_rr(600);
    CHECK(hm().hrv_valid && hm().hrv_rmssd_ms==0); /* 新稳态可重新捕获 */
    health_metrics_reset();
    double seconds=0;
    for (int i=0;i<100;i++) {
        float rr=(float)(800+40*sin(2*PI*0.25*seconds));
        health_metrics_feed_rr(rr); seconds+=rr/1000.0;
    }
    CHECK(hm().hrv_valid && hm().resp_valid);
    CHECK(fabsf(hm().resp_rate_bpm-15)<1.5f);
}
static void test_ecg(void)
{
    const int rates[]={100,300,334,360,388,500,800,1000,2000};
    float ecg[ARR_PP_WIN_LEN], rr[ARR_PP_RR_DIM];
    CHECK(!arr_preproc_set_sample_rate(NAN));
    CHECK(!arr_preproc_set_sample_rate(0));
    CHECK(!arr_preproc_set_sample_rate(2001));
    for (int k=0;k<9;k++) {
        int fs=rates[k], beats=0, measured=0;
        arr_preproc_reset(); CHECK(arr_preproc_set_sample_rate((float)fs));
        uint32_t prev=0;
        for (int i=0;i<fs*20;i++) {
            double phase=fmod((double)i/fs,1.0)-0.5;
            float sample=(float)(exp(-phase*phase/(2*0.015*0.015))
                              -0.2*exp(-(phase-0.035)*(phase-0.035)/(2*0.012*0.012)));
            beats+=arr_preproc_push_500(sample,ecg,rr);
            uint32_t count=arr_preproc_rpeak_count();
            if (count!=prev && count>4) {
                CHECK(fabsf(arr_preproc_last_rr_ms()-1000)<12);
                measured++;
            }
            prev=count;
        }
        printf("ECG fs=%d peaks=%u crops=%d RR=%gms\n",fs,prev,beats,arr_preproc_last_rr_ms());
        CHECK(beats>=12 && measured>=12);
        CHECK(arr_preproc_set_sample_rate(fs==500 ? 388.0f : 500.0f));
        CHECK(arr_preproc_rpeak_count()==0 && arr_preproc_last_rr_ms()==0);
    }
}
static void test_gsr(void)
{
    gsr_state_t st; float stress;
    gsr_state_reset(&st);
    for(int i=0;i<GSR_LEARN_SAMPLES-1;i++) CHECK(!gsr_process_sample(&st,1000,&stress));
    CHECK(gsr_process_sample(&st,1000,&stress) && stress==0);
    const float invalid[]={NAN,INFINITY,-INFINITY,-1,0,2047,4096};
    for(int j=0;j<7;j++) {
        CHECK(!gsr_process_sample(&st,invalid[j],&stress) && stress==0 && !st.learned);
        for(int i=0;i<GSR_LEARN_SAMPLES-1;i++) CHECK(!gsr_process_sample(&st,800,&stress));
        CHECK(gsr_process_sample(&st,800,&stress) && stress==0 && st.baseline_mv==800);
    }
    st.sample_count=UINT32_MAX;
    CHECK(gsr_process_sample(&st,800,&stress) && st.sample_count==UINT32_MAX);
    CHECK(!gsr_process_sample(NULL,800,&stress));
    CHECK(!gsr_process_sample(&st,800,NULL));
    gsr_state_reset(NULL);
}
static void test_session(void)
{
    ppg_session_t s;
    ppg_sqi_t good={0}; good.dc=40000; good.score=100;
    for(int state=PPG_SESSION_WAIT_FINGER;state<=PPG_SESSION_DONE;state++) {
        ppg_session_start(&s,100);
        s.state=(ppg_session_state_t)state; s.n_frames=2; s.hr_result=88;
        ppg_session_cancel(&s);
        CHECK(s.state==PPG_SESSION_CANCELLED && s.n_frames==0 && s.hr_result==0);
        CHECK(ppg_session_feed(&s,&good,PPG_GATE_PASS,75,98,0,RT_FALSE)==PPG_SESSION_ACT_NONE);
        CHECK(s.state==PPG_SESSION_CANCELLED);
        CHECK(!ppg_session_check_timeout(&s,200000));
    }
    ppg_session_start(&s,UINT32_MAX-44999);
    CHECK(!ppg_session_check_timeout(&s,44999));
    CHECK(ppg_session_check_timeout(&s,45000));
    CHECK(s.state==PPG_SESSION_FAILED && s.last_reject==PPG_GATE_REJ_TIMEOUT);
    ppg_session_start(&s,1000);
    CHECK(s.state==PPG_SESSION_WAIT_FINGER && s.last_reject==PPG_GATE_PASS);
    ppg_session_feed(&s,&good,PPG_GATE_PASS,75,98,0,RT_FALSE);
    CHECK(s.state==PPG_SESSION_SETTLING);
    ppg_session_feed(&s,&good,PPG_GATE_PASS,75,98,0,RT_FALSE);
    CHECK(s.state==PPG_SESSION_MEASURING);
    ppg_session_feed(&s,&good,PPG_GATE_REJ_MOTION,75,98,0,RT_FALSE);
    for(int i=0;i<3;i++) {
        int action=ppg_session_feed(&s,&good,PPG_GATE_PASS,75+i,98,0,RT_FALSE);
        CHECK(action==(i==2?PPG_SESSION_ACT_COMPLETED:PPG_SESSION_ACT_PROGRESS));
        CHECK(s.last_reject==PPG_GATE_PASS);
    }
    CHECK(s.state==PPG_SESSION_DONE && s.hr_result==76);
    CHECK(!ppg_session_check_timeout(&s,200000));
    ppg_session_start(&s,200000);
    CHECK(s.n_frames==0 && s.hr_result==0);
    good.dc=0;
    for(int i=0;i<PPG_SESSION_WAIT_FINGER_ROUNDS;i++) ppg_session_feed(&s,&good,PPG_GATE_REJ_NO_FINGER,0,0,0,RT_FALSE);
    CHECK(s.state==PPG_SESSION_FAILED && s.last_reject==PPG_GATE_REJ_NO_FINGER);
}
int main(void)
{
    test_ppg(); test_sqi(); test_hrv(); test_ecg(); test_gsr(); test_session();
    printf("Accuracy SIL: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
