/*
 * arrhythmia_task.c — M55 心律不齐推理任务(主 AI ①)
 *
 * 线程 "arrhyth"，priority=20，stack=4096(堆分配,不占 DTCM BSS)。
 * 职责: 启动延迟后初始化 NPU 引擎 → 驱动 ECG 前处理+推理 → 维护 PVC/min。
 *
 * 三级回退(同 glucose,见 memory 2026-socc-tflite-port-pattern):
 *   L1 NPU(Vela) → (L2 CPU 不可用) → L3 规则(仅 R-R 时序,粗判)
 *
 * ECG-IPC 连续性、采样率或来源中断时清除前处理和统计窗口。
 */

#include <rtthread.h>
#include <board.h>
#include "arrhythmia_task.h"
#include "arrhythmia_preproc.h"
#include "arrhythmia_inference.h"
#include "health_metrics.h"
#include "ui/ipc_shared_data.h"

#define DBG_TAG "arrhyth"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define ARR_THREAD_STACK  4096
#define ARR_THREAD_PRIO   20
#define ARR_INIT_DELAY_MS 15000   /* 等 NPU/系统稳定 */
#define ARR_POLL_MS       20      /* ECG 轮询周期 */
#define ARR_PVC_WIN_MS    60000   /* PVC/min 统计窗 */

static arr_result_t s_result;     /* 全 0 初始化: valid=0 */
static rt_tick_t s_pvc_start;
static uint32_t s_pvc_prev;
static int s_pvc_active;
static volatile uint32_t s_requested_session;
static uint32_t s_active_session;

uint32_t arrhythmia_request_session(void)
{
    rt_base_t level=rt_hw_interrupt_disable();
    uint32_t id=s_requested_session+1;
    if (!id) id=1;
    s_requested_session=id;
    rt_hw_interrupt_enable(level);
    return id;
}

void arrhythmia_copy_result(arr_result_t *out)
{
    rt_base_t level=rt_hw_interrupt_disable();
    if (out) *out=s_result;
    rt_hw_interrupt_enable(level);
}

const arr_result_t *arrhythmia_get_result(void)
{
    return &s_result;
}

/* ── L3 规则回退(仅 R-R 时序,无形态学,粗判安全网) ────────────────── */
static int l3_fallback(const float rr[3])
{
    /* rr[k]=clip(ratio,0,2)/2 → ratio=rr[k]*2 */
    float pre_local = rr[2] * 2.0f;   /* pre/local: <1 = 提前出现 */
    float post_med  = rr[1] * 2.0f;   /* post/med:  >1 = 代偿间歇 */
    if (pre_local < 0.85f)            /* 提前拍 */
        return (post_med > 1.15f) ? 2 /* V: 有代偿间歇 */ : 1 /* S */;
    return 0;                         /* N */
}

/* ── 单拍处理: L1 NPU 优先,失败 L3 ──────────────────────────────── */
static void process_beat(const float ecg[ARR_ECG_LEN], const float rr[ARR_RR_DIM])
{
    int   cls = -1;
    float conf = 0.0f;
    float probs[ARR_N_CLASS] = { 0 };
    int   src = ARR_SRC_FALLBACK;

    if (arrhythmia_inference_is_ready() &&
        arrhythmia_inference_invoke(ecg, rr, &cls, &conf, probs) == 0)
    {
        src = ARR_SRC_NPU;
    }
    else
    {
        cls = l3_fallback(rr);
        conf = 0.0f;
        for (int i = 0; i < ARR_N_CLASS; i++) probs[i] = 0.0f;
        if (cls >= 0 && cls < ARR_N_CLASS) probs[cls] = 1.0f;
    }

    if (cls < 0 || cls >= ARR_N_CLASS) return;

    arr_result_t result=s_result;
    result.last_class = cls;
    result.last_conf  = conf;
    for (int i = 0; i < ARR_N_CLASS; i++) result.probs[i] = probs[i];
    result.beat_count++;
    result.cls_count[cls]++;
    result.source = src;
    result.tick   = rt_tick_get();
    result.valid  = 1;
    if (!s_pvc_active) {
        s_pvc_active=1; s_pvc_start=result.tick;
        s_pvc_prev=result.cls_count[2]-(cls==2);
    }
    rt_base_t level=rt_hw_interrupt_disable();
    s_result=result;
    rt_hw_interrupt_enable(level);
}

int arrhythmia_feed_500hz(const float *samples, int n)
{
    if (samples == RT_NULL || n <= 0) return 0;
    float ecg[ARR_ECG_LEN], rr[ARR_RR_DIM];
    int beats = 0;
    for (int i = 0; i < n; i++)
    {
        uint32_t prev=arr_preproc_rpeak_count();
        int ready=arr_preproc_push_500(samples[i],ecg,rr);
        if (arr_preproc_rpeak_count()!=prev && arr_preproc_last_rr_ms()>0)
            health_metrics_feed_rr(arr_preproc_last_rr_ms());
        if (ready) { process_beat(ecg,rr); beats++; }
    }
    return beats;
}

/* ── ECG-IPC 读取(M33 ecg_task 连续写环形缓冲，这里追赶消费) ──────────
 * ecg_waveform_ipc_t 是环形缓冲 + 单调递增 write_seq，不是"整块覆盖"，
 * 所以要维护本地 s_ecg_read_seq 追赶读取，而不是像 PPG 那样只看 sequence
 * 变没变。追赶不及(被 M33 套圈)时丢弃过旧数据，从当前可读的最老一份续上。
 */
static rt_uint32_t s_ecg_read_seq = 0;

static uint32_t s_epoch, s_rate, s_write_seen;
static rt_tick_t s_last_ecg;
static void reset_continuity(void)
{
    arr_preproc_reset(); health_metrics_reset();
    s_pvc_active=0; s_pvc_prev=0;
    rt_base_t level=rt_hw_interrupt_disable();
    rt_memset(&s_result,0,sizeof(s_result));
    s_result.session_id=s_active_session;
    rt_hw_interrupt_enable(level);
}

static void accept_session(uint32_t write_seq)
{
    s_active_session=s_requested_session;
    reset_continuity();
    s_ecg_read_seq=write_seq;
}

static void update_pvc_window(rt_tick_t now)
{
    rt_tick_t elapsed=now-s_pvc_start;
    if (!s_pvc_active || elapsed < rt_tick_from_millisecond(ARR_PVC_WIN_MS)) return;
    uint32_t v_now=s_result.cls_count[2];
    uint32_t rate=(uint32_t)(((uint64_t)(v_now-s_pvc_prev)*
        rt_tick_from_millisecond(ARR_PVC_WIN_MS)+elapsed/2)/elapsed);
    rt_base_t level=rt_hw_interrupt_disable();
    s_result.pvc_per_min=rate; s_result.pvc_valid=1;
    rt_hw_interrupt_enable(level);
    s_pvc_start=now; s_pvc_prev=v_now;
    if (s_result.valid)
        LOG_I("beats=%u N=%u S=%u V=%u F=%u PVC/min=%u [%s]",
              s_result.beat_count, s_result.cls_count[0], s_result.cls_count[1],
              s_result.cls_count[2], s_result.cls_count[3], s_result.pvc_per_min,
              s_result.source == ARR_SRC_NPU ? "NPU" : "RULE");
}

static int arr_read_ecg_ipc(float *buf,int max_n)
{
    volatile ecg_waveform_ipc_t *ipc=ipc_get_ecg_ptr();
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((uint32_t *)IPC_ECG_WAVEFORM_ADDR,sizeof(*ipc));
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t write_seq=ipc->write_seq, epoch=ipc->stream_epoch, hz=ipc->sample_rate_hz;
    rt_tick_t now=rt_tick_get();
    if (ipc->magic!=IPC_ECG_MAGIC || hz<100 || hz>2000) {
        reset_continuity(); s_ecg_read_seq=write_seq; return 0;
    }
    if (write_seq!=s_write_seen) { s_write_seen=write_seq; s_last_ecg=now; }
    if (now-s_last_ecg>rt_tick_from_millisecond(100)) {
        reset_continuity(); s_ecg_read_seq=write_seq; return 0;
    }
    if (epoch!=s_epoch || hz!=s_rate) {
        s_epoch=epoch; s_rate=hz;
        arr_preproc_set_sample_rate((float)hz);
        reset_continuity(); s_ecg_read_seq=write_seq; return 0;
    }
    uint32_t avail=write_seq-s_ecg_read_seq;
    if (avail>=ECG_IPC_RING_LEN) {
        reset_continuity(); s_ecg_read_seq=write_seq; return 0;
    }
    if (avail>(uint32_t)max_n) avail=max_n;
    for (uint32_t i=0;i<avail;i++) buf[i]=ipc->ring[(s_ecg_read_seq+i)%ECG_IPC_RING_LEN];
    /* Reject a copy that was lapped or crossed a producer discontinuity. */
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((uint32_t *)IPC_ECG_WAVEFORM_ADDR,sizeof(*ipc));
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    if (ipc->stream_epoch!=epoch || ipc->sample_rate_hz!=hz || ipc->magic!=IPC_ECG_MAGIC ||
        ipc->write_seq-s_ecg_read_seq>=ECG_IPC_RING_LEN) {
        reset_continuity(); s_ecg_read_seq=ipc->write_seq; return 0;
    }
    s_ecg_read_seq+=avail;
    return (int)avail;
}

static void arrhythmia_thread_entry(void *param)
{
    RT_UNUSED(param);

    LOG_I("arrhythmia task started, waiting %d ms for NPU", ARR_INIT_DELAY_MS);
    rt_thread_mdelay(ARR_INIT_DELAY_MS);

    arr_preproc_reset();
    health_metrics_reset();

    int ret = arrhythmia_inference_init();
    if (ret == 0)
        LOG_I("NPU inference ready, arena_used=%u bytes",
              (unsigned)arrhythmia_inference_arena_used());
    else
        LOG_W("NPU init failed (ret=%d), beats will use L3 rule fallback", ret);

    static float ecg_blk[64];

    /* TEMP DEBUG：确认 ECG-IPC 真的有连续数据流进来(跟"有没有检测到 R 峰"分开排查) */
    uint32_t dbg_total_samples = 0;
    uint32_t dbg_elapsed = 0;

    while (1)
    {
        rt_thread_mdelay(ARR_POLL_MS);

        if (s_requested_session != s_active_session) {
            volatile ecg_waveform_ipc_t *ipc=ipc_get_ecg_ptr();
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
            SCB_InvalidateDCache_by_Addr((uint32_t *)ipc, sizeof(*ipc));
#endif
            __asm__ volatile("dsb sy" ::: "memory");
            accept_session(ipc->write_seq); /* 请求前样本不进入本次体检 */
            continue;
        }

        /* 从 ECG-IPC 追赶连续样本并驱动推理。 */
        int n = arr_read_ecg_ipc(ecg_blk, sizeof(ecg_blk) / sizeof(ecg_blk[0]));
        if (n > 0)
            arrhythmia_feed_500hz(ecg_blk, n);

        dbg_total_samples += (n > 0) ? (uint32_t)n : 0;
        dbg_elapsed += ARR_POLL_MS;
        if (dbg_elapsed >= 5000)
        {
            LOG_I("[dbg] ecg samples consumed=%u (last poll n=%d), beat_count=%u, valid=%d",
                  dbg_total_samples, n, s_result.beat_count, s_result.valid);
            dbg_elapsed = 0;
        }

        if (s_result.valid && rt_tick_get()-s_result.tick>rt_tick_from_millisecond(3000)) {
            reset_continuity();
        }
        /* 统计完整连续窗口，使用真实 tick，含推理/调度耗时。 */
        update_pvc_window(rt_tick_get());
    }
}

static int arrhythmia_task_init(void)
{
    rt_thread_t tid = rt_thread_create(
        "arrhyth", arrhythmia_thread_entry, RT_NULL,
        ARR_THREAD_STACK, ARR_THREAD_PRIO, 10);
    if (tid == RT_NULL)
    {
        LOG_E("arrhythmia thread create failed");
        return -RT_ERROR;
    }
    rt_thread_startup(tid);
    LOG_I("arrhythmia task created (prio=%d, stack=%d)",
          ARR_THREAD_PRIO, ARR_THREAD_STACK);
    return RT_EOK;
}

/*
 * [2026-06-15 诊断隔离已解除，2026-07-08 重新开启]
 * 当时为排查 M55 黑屏关掉过开机自启；真根因后来确认是 uart2/SCB2 双核共享
 * 竞争（跟本任务无关，见 memory 2026-socc-m55-toolchain-regression 第4节）+
 * NPU 中断向量从未真正挂载（同上第5节），均已修复，此处恢复正常开机自启。
 */
INIT_APP_EXPORT(arrhythmia_task_init);
