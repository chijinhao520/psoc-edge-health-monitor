/*
 * health_metrics.c — HRV(RMSSD/SDNN) + 呼吸率 实现
 *
 * 见 health_metrics.h 顶部说明：呼吸率只用 RSA(频率调制)一路特征，
 * 是 Karlen 2013 的简化版。
 */

#include "health_metrics.h"
#include <string.h>
#include <math.h>
#if defined(__arm__) || defined(__thumb__)
#include <rtthread.h>
#define HM_LOCK() rt_base_t level = rt_hw_interrupt_disable()
#define HM_UNLOCK() rt_hw_interrupt_enable(level)
#else
#define HM_LOCK() ((void)0)
#define HM_UNLOCK() ((void)0)
#endif

#define HM_RING_LEN          128   /* 环形缓冲长度，覆盖约 1 分钟(心率 60-100 时) */
#define HM_MIN_HRV_SAMPLES   10   /* HRV 至少要这么多个 R-R 间期才认为可信 */
#define HM_MIN_RESP_SAMPLES  20   /* 呼吸率需要更长窗口才稳 */
#define HM_TREND_HALF_WIN    5    /* 去趋势窗半宽(拍)，约 8-11 拍覆盖一个典型呼吸周期 */

/*
 * ★ 异位搏动(早搏)跳变阈值 —— HRV 的 NN 是 "normal-to-normal"，本就定义为
 * 剔除异位搏动后的间期。室早(PVC)会造成"一个短 RR + 一个长代偿 RR"这一对，
 * 不剔除的话 RMSSD 会被单次早搏抬到天上，HRV 就退化成早搏计数器。
 *
 * 2026-07-17 PC 仿真实测(scratchpad/hrv_sim.py，80 拍含 3 次早搏)：
 *     不过滤 → RMSSD = 173.8ms（虚高 **6.8 倍**）
 *     过滤后 → RMSSD = 25.8ms，与无早搏基准 25.5ms 仅差 1.1%
 * 这对本项目尤其要命：主 AI 就是心律不齐检测，用户群体本就可能有早搏。
 *
 * 20% 是 Task Force 1996 指南沿用的常规阈值。
 */
#define HM_RR_JUMP_FRAC      0.20f

/* 呼吸率生理闸。原实现只要 crossings>0 就置 resp_valid=1，
 * 1 次过零/20 秒 = 3 rpm 这种荒谬值照样放行 */
#define HM_RESP_MIN_RPM      8.0f
#define HM_RESP_MAX_RPM      30.0f

static float    s_rr_ring[HM_RING_LEN];
static int      s_rr_idx;
static int      s_rr_count;       /* 环内实际样本数，饱和于 HM_RING_LEN */
static uint32_t s_total_rr_fed;   /* 累计喂入总数(诊断用，不受环大小限制) */
static float    s_last_nn;        /* 上一个被接受的 NN，用于跳变判定 */

static float s_candidate;
static int s_candidate_count;
static health_metrics_result_t s_result;

static void publish(const health_metrics_result_t *value)
{
    HM_LOCK();
    s_result = *value;
    HM_UNLOCK();
}

static void clear_window(void)
{
    health_metrics_result_t empty = {0};
    s_rr_idx = 0;
    s_rr_count = 0;
    empty.rr_count = s_total_rr_fed;
    publish(&empty);
}

void health_metrics_reset(void)
{
    memset(s_rr_ring, 0, sizeof(s_rr_ring));
    s_rr_idx = 0;
    s_rr_count = 0;
    s_total_rr_fed = 0;
    s_last_nn = 0.0f;
    s_candidate = 0.0f;
    s_candidate_count = 0;
    clear_window();
}

/* 按时间顺序(从最老到最新)还原环形缓冲到 out[]，返回样本数 */
static int ordered_rr(float out[HM_RING_LEN])
{
    int n = s_rr_count;
    int start = (s_rr_count < HM_RING_LEN) ? 0 : s_rr_idx;
    for (int i = 0; i < n; i++)
        out[i] = s_rr_ring[(start + i) % HM_RING_LEN];
    return n;
}

static void recompute(void)
{
    health_metrics_result_t result = {0};
    float ordered[HM_RING_LEN];
    float duration_ms = 0.0f;
    int n = ordered_rr(ordered);

    if (n < 2)
        return;

    for (int j = 0; j < n; j++) duration_ms += ordered[j];

    /* --- SDNN --- */
    float mean = 0.0f;
    for (int i = 0; i < n; i++) mean += ordered[i];
    mean /= (float)n;

    float var = 0.0f;
    for (int i = 0; i < n; i++)
    {
        float d = ordered[i] - mean;
        var += d * d;
    }
    var /= (float)(n - 1);
    result.hrv_sdnn_ms = sqrtf(var);

    /* --- RMSSD --- */
    float sq_sum = 0.0f;
    for (int i = 1; i < n; i++)
    {
        float d = ordered[i] - ordered[i - 1];
        sq_sum += d * d;
    }
    result.hrv_rmssd_ms = sqrtf(sq_sum / (float)(n - 1));
    result.hrv_valid = (n >= HM_MIN_HRV_SAMPLES && duration_ms >= 30000.0f) ? 1 : 0;

    /* --- 呼吸率(RSA 简化法) --- */
    if (n >= HM_MIN_RESP_SAMPLES && duration_ms >= 30000.0f)
    {
        /*
         * ★ 此处原有一道 3 点移动平均"去噪"，2026-07-17 删除（PC 仿真裁决）。
         *
         * 它的频响 H(f) = (1 + 2cos(2πf/fs)) / 3 在 f = fs/3 处**恰好为 0**，
         * 而 R-R 序列的采样率 fs 就是心率(Hz)：
         *     @60bpm  fs=1.00Hz → 零点 = **20 rpm**  ← 正常呼吸范围正中间!
         *     @75bpm  fs=1.25Hz → 零点 = **25 rpm**
         * 即它制造了一个**随心率漂移的呼吸率盲区**。仿真实测(hrv_sim.py)：
         *     @75bpm/25rpm → 误差 -16.2 rpm（信号被滤没，数到的是噪声假过零）
         *     @60bpm/20rpm → 直接不出数
         * 删掉后全频段误差收敛到 ≤1.5 rpm，且**连抗噪都更好**
         * （noise=8ms 时 3点MA 在 20~28rpm 段误差 -3.4~-16.1，无平滑仍 ≤-1.5）
         * —— 因为下面的滞回双门限本身就是强抗噪机制。
         *
         * 教训：别凭"加个平滑更稳"的直觉往信号链塞滤波器，先算它的频响零点落在哪。
         *
         * 下面的**局部去趋势保留** —— 它用 ±HM_TREND_HALF_WIN 的滑动均值做基线，
         * 能跟随心率慢漂移，比减全局均值稳。
         */
        float detr[HM_RING_LEN];
        float amp_sum = 0.0f;

        for (int i = 0; i < n; i++)
        {
            int a = i - HM_TREND_HALF_WIN; if (a < 0) a = 0;
            int b = i + HM_TREND_HALF_WIN; if (b >= n) b = n - 1;

            float trend = 0.0f;
            for (int k = a; k <= b; k++) trend += ordered[k];
            trend /= (float)(b - a + 1);

            detr[i] = ordered[i] - trend;   /* 去趋势后近似只剩呼吸频段起伏 */
            amp_sum += (detr[i] < 0.0f) ? -detr[i] : detr[i];
        }

        /*
         * 幅度门限 + 滞回：只有起伏够大的过零才算数。
         * 原实现直接判 prev<=0 && cur>0，噪声在 0 附近抖动会被反复计数
         * （resp_valid 又只要 crossings>0 就置 1，于是噪声能直接变成"呼吸率"）。
         */
        float amp_mean = amp_sum / (float)n;
        int   crossings = 0;

        if (amp_mean >= 1.0f)      /* R-R 几乎不动 = 无 RSA 信号(或机械节律) */
        {
            float thr = amp_mean * 0.25f;
            int   armed = 0;       /* 1 = 已跌破下门限，等待上穿 */
            for (int i = 0; i < n; i++)
            {
                if (detr[i] < -thr)
                    armed = 1;
                else if (armed && detr[i] > thr)
                {
                    crossings++;   /* 一个完整呼吸周期 */
                    armed = 0;
                }
            }
        }

        float total_sec = 0.0f;
        for (int i = 0; i < n; i++) total_sec += ordered[i] / 1000.0f;

        /*
         * 出数条件收紧：
         *   - crossings >= 2：不足 2 个完整呼吸周期不给数（原来 >0 就给）
         *   - 生理闸 + ★ Nyquist 动态闸：R-R 序列每拍一个采样点 → 采样率 = 心率，
         *     Nyquist = 心率/2，呼吸率超过它必然混叠。固定上限在低心率下不安全：
         *     @50bpm Nyquist 仅 25rpm。取 0.4×心率(= 0.8×Nyquist)留余量：
         *     @50bpm→20rpm，@60bpm→24rpm，@75bpm→30rpm。这是物理限制，非可调参数。
         */
        if (crossings >= 2 && total_sec > 1.0f && mean > 0.0f)
        {
            float rpm     = (float)crossings / total_sec * 60.0f;
            float mean_hr = 60000.0f / mean;
            float hi      = mean_hr * 0.4f;
            if (hi > HM_RESP_MAX_RPM) hi = HM_RESP_MAX_RPM;

            if (rpm >= HM_RESP_MIN_RPM && rpm <= hi)
            {
                result.resp_rate_bpm = rpm;
                result.resp_valid = 1;
            }
            else
            {
                result.resp_valid = 0;   /* 超范围 = 过零来自伪迹，宁可不出数 */
            }
        }
        else
        {
            result.resp_valid = 0;
        }
    }
    else
    {
        result.resp_valid = 0;
    }

    result.rr_count = s_total_rr_fed;
    publish(&result);
}

void health_metrics_feed_rr(float rr_ms)
{
    /* 拒绝间期必须切断连续窗口，不能跨伪迹计算相邻差或压缩呼吸时间轴。 */
    if (!isfinite(rr_ms) || rr_ms < 300.0f || rr_ms > 2000.0f)
    {
        clear_window();
        s_candidate_count = 0;
        return;
    }
    if (s_last_nn > 0.0f && fabsf(rr_ms - s_last_nn) > s_last_nn * HM_RR_JUMP_FRAC)
    {
        clear_window();
        /* 三个相互接近的候选允许重新捕获，避免永久锁在旧心率。 */
        if (s_candidate_count && fabsf(rr_ms - s_candidate) <= s_candidate * 0.10f)
            s_candidate_count++;
        else
            s_candidate_count = 1;
        s_candidate = rr_ms;
        if (s_candidate_count < 3) return;
    }
    s_candidate_count = 0;
    s_last_nn = rr_ms;
    s_rr_ring[s_rr_idx] = rr_ms;
    s_rr_idx = (s_rr_idx + 1) % HM_RING_LEN;
    if (s_rr_count < HM_RING_LEN) s_rr_count++;
    s_total_rr_fed++;
    recompute();
}

void health_metrics_copy_result(health_metrics_result_t *out)
{
    if (out)
    {
        HM_LOCK();
        *out = s_result;
        HM_UNLOCK();
    }
}

const health_metrics_result_t *health_metrics_get_result(void)
{
    return &s_result;
}
