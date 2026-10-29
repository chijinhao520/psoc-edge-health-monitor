/*
 * PPG 信号质量评估 (SQI) + 生理合理性门控 — 实现
 *
 * 设计说明见 ppg_sqi.h 顶部。
 *
 * 内存策略：500 点 int32 工作数组 = 2KB，超过 ppg 线程 2048B 栈，
 * 故用 static（本模块只被 ppg_task 单线程调用，无重入需求）。
 * 与 ppg_process.c 的 static 工作数组同样的取舍。
 */

#include <rtthread.h>
#include <math.h>
#include "ppg_sqi.h"
#include "ppg_process.h"   /* BUFFER_SIZE / FS */

#define DBG_TAG "ppg_sqi"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 峰检测参数 */
#define SQI_MA_SIZE       4                  /* 移动平均窗口，同 maxim 的 MA4_SIZE */
#define SQI_MAX_PEAKS     20                 /* 5s @ 180bpm 上限 15 拍，留余量 */
#define SQI_MIN_PEAKS     3                  /* 少于 3 峰算不出可信的 IBI/幅值统计 */

/* 按连续样本时钟折算最小峰距；当前 FIFO 固定100SPS，与任务读取速度无关。
 * 质量统计保留400ms峰距，心率差分检测保留340ms；本次不调整经验门限。 */
static int32_t sqi_min_peak_dist(uint32_t fs)
{
    int32_t d = (int32_t)(fs * 60u / PPG_HR_MAX);
    return (d < 2) ? 2 : d;
}

static int32_t s_work[BUFFER_SIZE];          /* 去均值 + 平滑后的工作信号 */
static int32_t s_peak_locs[SQI_MAX_PEAKS];
static int32_t s_peak_amps[SQI_MAX_PEAKS];

/**
 * 在去均值信号上找局部极大值（真实脉搏波峰，非差分域的边沿）。
 *
 * 策略：先算自适应阈值（正半轴均值），再扫描局部极大，
 * 最后按最小间隔去重（保留先到的峰，与 maxim_remove_close_peaks 同思路）。
 */
static int32_t sqi_find_peaks(const int32_t *x, int32_t len,
                              int32_t *locs, int32_t *amps, int32_t max_n,
                              int32_t min_dist)
{
    int32_t i, n = 0;
    int32_t th = 0, pos_cnt = 0;
    int64_t pos_sum = 0;

    /* 自适应阈值：正半轴均值的一半。全负（无脉搏）时阈值为 0，扫不出峰 */
    for (i = 0; i < len; i++)
    {
        if (x[i] > 0)
        {
            pos_sum += x[i];
            pos_cnt++;
        }
    }
    if (pos_cnt == 0)
        return 0;
    th = (int32_t)(pos_sum / pos_cnt) / 2;

    for (i = 1; i < len - 1 && n < max_n; i++)
    {
        if (x[i] <= th)
            continue;
        if (x[i] <= x[i - 1])   /* 必须有严格上升沿，平台内部不能重复计峰 */
            continue;

        int32_t right = i;
        while (right + 1 < len && x[right + 1] == x[i]) right++;
        if (right + 1 == len || x[right + 1] >= x[i]) {
            i = right;
            continue;
        }
        /* 完整平顶只记中心一次；窗口末尾尚未下降的平台不算完整峰。 */
        int32_t peak = i + (right - i) / 2;
        i = right;

        /* 与上一个已接受的峰太近 → 保留更高的那个 */
        if (n > 0 && (peak - locs[n - 1]) < min_dist)
        {
            if (x[i] > amps[n - 1])
            {
                locs[n - 1] = peak;
                amps[n - 1] = x[i];
            }
            continue;
        }

        locs[n] = peak;
        amps[n] = x[i];
        n++;
    }

    return n;
}

rt_err_t ppg_sqi_compute(const uint32_t *ir, int32_t len, uint32_t fs, ppg_sqi_t *out)
{
    int64_t sum = 0;
    uint32_t dc;
    int32_t  i, n;
    int64_t  amp_sum = 0;
    float    amp_mean, amp_var_sum = 0.0f, amp_std;
    float    ibi_mean_ms, ibi_var_sum = 0.0f;
    float    ac;
    float    ms_per_sample;

    if (out == RT_NULL)
        return -RT_EINVAL;
    /* 即使输入错误也撤掉旧评分，不让调用者误用上一窗结果。 */
    rt_memset(out, 0, sizeof(*out));
    if (ir == RT_NULL || len < SQI_MA_SIZE + 2 || len > BUFFER_SIZE ||
        fs == 0 || fs > 1000)
        return -RT_EINVAL;
    for (i = 0; i < len; i++)
        if (ir[i] == 0 || ir[i] >= 0x3FFFFu) return -RT_EINVAL;
    ms_per_sample = 1000.0f / (float)fs;

    /* ---- 1. DC = 原始信号均值 ---- */
    for (i = 0; i < len; i++)
        sum += ir[i];
    dc = (uint32_t)(sum / len);
    out->dc = dc;
    if (dc == 0)
        return RT_EOK;   /* 全零信号 → score 0，让门控拒掉 */

    /* ---- 2. 去均值 + 4 点移动平均（抑制高频噪声，同 maxim 前处理） ---- */
    for (i = 0; i < len; i++)
        s_work[i] = (int32_t)ir[i] - (int32_t)dc;

    for (i = 0; i < len - SQI_MA_SIZE; i++)
        s_work[i] = (s_work[i] + s_work[i + 1] + s_work[i + 2] + s_work[i + 3]) / 4;

    /* ---- 3. 峰检测 ---- */
    n = sqi_find_peaks(s_work, len - SQI_MA_SIZE, s_peak_locs, s_peak_amps,
                       SQI_MAX_PEAKS, sqi_min_peak_dist(fs));
    out->n_peaks = n;

    if (n < SQI_MIN_PEAKS)
        return RT_EOK;   /* 峰太少，统计量无意义 → score 0 */

    /* ---- 4. 灌注指数 PI = AC/DC*100 ---- */
    /* AC 用峰幅均值的 2 倍近似峰峰值（去均值后波形大致对称于 0） */
    for (i = 0; i < n; i++)
        amp_sum += s_peak_amps[i];
    amp_mean = (float)amp_sum / (float)n;
    ac = amp_mean * 2.0f;
    out->pi = ac / (float)dc * 100.0f;

    /* ---- 5. 峰幅变异系数 CV = std/mean ---- */
    for (i = 0; i < n; i++)
    {
        float d = (float)s_peak_amps[i] - amp_mean;
        amp_var_sum += d * d;
    }
    /* 单精度库函数覆盖完整量程；原先从x开始固定12次迭代不能保证收敛。 */
    amp_std = sqrtf(amp_var_sum / (float)n);
    out->peak_amp_cv = (amp_mean > 0.0f) ? (amp_std / amp_mean) : 999.0f;

    /* ---- 6. 峰间隔标准差（ms）----
     * fs 是连续样本时钟，当前 FIFO 为 100SPS，不是任务读取频率。 */
    {
        float ibi_sum = 0.0f;
        int32_t n_ibi = n - 1;

        for (i = 1; i < n; i++)
            ibi_sum += (float)(s_peak_locs[i] - s_peak_locs[i - 1]) * ms_per_sample;
        ibi_mean_ms = ibi_sum / (float)n_ibi;

        for (i = 1; i < n; i++)
        {
            float ibi = (float)(s_peak_locs[i] - s_peak_locs[i - 1]) * ms_per_sample;
            float d   = ibi - ibi_mean_ms;
            ibi_var_sum += d * d;
        }
        out->ibi_std_ms = sqrtf(ibi_var_sum / (float)n_ibi);
    }

    /* ---- 7. 综合评分（扣分制，与 5/28 规划的 compute_ppg_sqi 一致）---- */
    out->score = 100;
    if (out->pi < PPG_SQI_PI_MIN || out->pi > PPG_SQI_PI_MAX)
        out->score -= 40;                       /* 灌注差(没贴稳/冰手) 或 基线漂移/伪迹(PI 虚高) */
    if (out->ibi_std_ms > PPG_SQI_IBI_STD_MAX_MS)
        out->score -= 30;                       /* IBI 跳动：峰检测被伪迹带偏 */
    if (out->peak_amp_cv > PPG_SQI_AMP_CV_MAX)
        out->score -= 30;                       /* 峰幅波动：手指在动 */
    if (out->score < 0)
        out->score = 0;

    return RT_EOK;
}

ppg_gate_result_t ppg_sqi_gate(const ppg_sqi_t *sqi,
                               int32_t hr, int32_t spo2,
                               int8_t hr_valid, int8_t spo2_valid)
{
    /* 闸 0：手指在位（DC 门限）。放最前面 —— 手指不在时后面所有指标都无意义，
     * 且这是唯一能干净区分"没放手指"的判据（实测 3.4k vs 42k，见 ppg_sqi.h）。
     * maxim 算法在纯噪声上照样能算出 hr=250/spo2=99 并置 valid=1，
     * 生理范围闸也拦不住恰好落在 40-180 内的噪声（实测 hr=171 漏网）。 */
    if (sqi == RT_NULL || sqi->dc < PPG_DC_FINGER_MIN)
        return PPG_GATE_REJ_NO_FINGER;

    if (!isfinite(sqi->pi) || !isfinite(sqi->ibi_std_ms) ||
        !isfinite(sqi->peak_amp_cv) || sqi->pi < 0.0f ||
        sqi->ibi_std_ms < 0.0f || sqi->peak_amp_cv < 0.0f ||
        sqi->n_peaks < SQI_MIN_PEAKS || sqi->n_peaks > SQI_MAX_PEAKS ||
        sqi->score < 0 || sqi->score > 100)
        return PPG_GATE_REJ_SQI;

    /* 闸 1：算法自己说无效 */
    if (!hr_valid || !spo2_valid)
        return PPG_GATE_REJ_ALGO;

    /* 闸 2：超出当前支持范围时拒绝输出，不能据此诊断或排除真实异常。 */
    if (spo2 < PPG_SPO2_MIN || spo2 > PPG_SPO2_MAX)
        return PPG_GATE_REJ_SPO2;
    if (hr < PPG_HR_MIN || hr > PPG_HR_MAX)
        return PPG_GATE_REJ_HR;

    /* 闸 3a：运动伪迹。PI 超上限说明低频漂移压过了脉搏 AC ——
     * 这类帧 maxim 照样能算出"看着正常"的心率(实测 41/104/133 乱跳)，
     * 必须拦。单独返回是为了让 UI 能提示"手指别动"，见枚举注释。
     * 放在 score 判据之前：score 是三项扣分的合计，拦下来也说不清是哪项。 */
    if (sqi->pi > PPG_SQI_PI_MAX)
        return PPG_GATE_REJ_MOTION;

    /* 闸 3b：SQI 软判据，拦"手指在位但波形不可用"的帧（灌注差/峰幅乱） */
    if (sqi->score < PPG_SQI_ACCEPT_SCORE)
        return PPG_GATE_REJ_SQI;

    return PPG_GATE_PASS;
}

const char *ppg_gate_reason_str(ppg_gate_result_t r)
{
    switch (r)
    {
    case PPG_GATE_PASS:      return "pass";
    case PPG_GATE_REJ_NO_FINGER: return "no-finger";
    case PPG_GATE_REJ_ALGO:  return "algo-invalid";
    case PPG_GATE_REJ_SPO2:  return "spo2-out-of-range";
    case PPG_GATE_REJ_HR:    return "hr-out-of-range";
    case PPG_GATE_REJ_SQI:   return "low-sqi";
    case PPG_GATE_REJ_MOTION: return "motion-artifact";
    case PPG_GATE_REJ_SENSOR: return "sensor-unavailable";
    case PPG_GATE_REJ_TIMEOUT: return "measurement-timeout";
    default:                 return "unknown";
    }
}
