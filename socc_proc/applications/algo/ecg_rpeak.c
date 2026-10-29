/*
 * ecg_rpeak.c — M33 侧 ECG R 峰计时实现
 *
 * 核心算法逐字复用 Edgi_Talk_M55_LVGL/applications/arrhythmia_preproc.c
 * （已在 MIT-BIH 验证: R 峰 Se/PPV 99.9-100%）。所有阈值/系数/窗长与
 * M55 版强一致，见 ecg_rpeak.h 顶部注释说明两份拷贝的用途分工。
 */

#include "ecg_rpeak.h"
#include <math.h>
#include <string.h>

/* ── 常量(与 M55 版一致) ─────────────────────────────────── */
#define FS            ARR_PP_FS                 /* 360 */
#define WIN_HALF      128
#define WIN_LEN       ARR_PP_WIN_LEN            /* 256 */
#define MWI_W         54                        /* round(0.150*360) */
#define REFR          72                        /* 0.200*360 不应期 */
#define BACK          (MWI_W + 30)              /* R 精定位回溯窗 ~因果群延迟 */
#define LEARN_N       720                       /* 2s 学习期 */
#define RR_LOCAL_K    10

#define RAW_RING      1024                      /* 原始 360 信号环(>=最大RR+窗+余量) */
#define MWI_RING      512                       /* MWI 环(searchback 回扫用) */
#define RHIST         64                        /* R 峰位置历史 */
#define RR_RING       32                        /* 近期 RR(运行中位数) */

/* butter(2,[5,15],'bandpass',fs=360) → sos,DF2T。{b0,b1,b2,a1,a2}(a0=1) */
static const float SOS[2][5] = {
    { 0.0067654133f,  0.0135308265f,  0.0067654133f, -1.7919338332f, 0.8412220576f },
    { 1.0000000000f, -2.0000000000f,  1.0000000000f, -1.9193726171f, 0.9287446452f },
};

/* ── 状态 ─────────────────────────────────────────────────────────── */
/* 重采样 */
static float  s_rs_prev;
static double s_rs_acc;
static float s_input_hz = ARR_PP_FS_IN;
static int    s_rs_have_prev;
/* 带通 biquad 状态 */
static float  s_bq_z1[2], s_bq_z2[2];
/* 5 点导数移位寄存器(最近 5 个 bp) */
static float  s_bp[5];
/* 因果 MWI */
static float  s_sq_ring[MWI_W];
static int    s_sq_idx;
static float  s_sq_sum;
/* MWI 历史环(searchback) */
static float  s_mwi_ring[MWI_RING];
/* 原始 360 信号环 */
static float  s_raw[RAW_RING];
/* 全局 360 采样计数 */
static long   s_n360;
/* 学习期 */
static int    s_learn;
static float  s_learn_max, s_learn_sum;
static long   s_learn_cnt;
/* 阈值 */
static float  s_SPKI, s_NPKI;
static long   s_last_qrs;
static int    s_rr_avg;
/* MWI 峰跟踪(检测 g-1 处局部极大) */
static float  s_mwi_prev, s_mwi_pprev;
static int    s_mwi_have;
/* R 历史 */
static long   s_rhist[RHIST];
static long   s_rcount;
/* RR 环(中位数) */
static int    s_rr_arr[RR_RING];
static int    s_rr_idx, s_rr_cnt;
/* 最近一次 R-R 间期(ms)，供 HRV/呼吸率用 */
static float  s_last_rr_ms;
/* 待产出心拍 */
static int    s_pend_active;
static long   s_pend_r;
static long   s_pend_emit_at;
static float  s_pend_rr[ARR_PP_RR_DIM];
/* 产出缓冲 */
static float  s_beat_ecg[WIN_LEN];

/* ── 工具 ─────────────────────────────────────────────────────────── */
static inline float clip2_half(float x)         /* clip(x,0,2)/2 → [0,1] */
{
    if (x < 0.0f) x = 0.0f;
    if (x > 2.0f) x = 2.0f;
    return x * 0.5f;
}

static float median_rr(void)
{
    int n = s_rr_cnt;
    if (n <= 0) return (float)(int)(0.8f * FS);
    int tmp[RR_RING];
    memcpy(tmp, s_rr_arr, sizeof(int) * n);
    for (int i = 1; i < n; i++)        /* 插入排序(n<=32) */
    {
        int v = tmp[i], j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = v;
    }
    return (n & 1) ? (float)tmp[n / 2]
                   : 0.5f * (float)(tmp[n / 2 - 1] + tmp[n / 2]);
}

/* R 精定位:原始信号窗 [pidx-BACK, current] 内幅值极大(去局部均值) */
static long refine_r(long pidx)
{
    long a = pidx - BACK;
    long b = s_n360;                    /* 当前已存到 s_raw[s_n360] */
    long oldest = s_n360 - RAW_RING + 1;
    if (a < oldest) a = oldest;
    if (a < 0) a = 0;
    if (b - a < 2) return pidx;

    float mean = 0.0f;
    for (long k = a; k <= b; k++) mean += s_raw[((k % RAW_RING) + RAW_RING) % RAW_RING];
    mean /= (float)(b - a + 1);

    long best = a; float bestv = -1.0f;
    for (long k = a; k <= b; k++)
    {
        float v = s_raw[((k % RAW_RING) + RAW_RING) % RAW_RING] - mean;
        if (v < 0) v = -v;
        if (v > bestv) { bestv = v; best = k; }
    }
    return best;
}

/* 登记一个 R(精定位 + 历史 + RR环 + 设置待产出),不动阈值 */
static void register_r(long pidx)
{
    long r = refine_r(pidx);
    long pos = s_rcount;
    s_rhist[pos % RHIST] = r;
    s_rcount++;

    if (pos >= 1)
    {
        long prevR = s_rhist[(pos - 1) % RHIST];
        int rr = (int)(r - prevR);
        if (rr > 0)
        {
            s_rr_arr[s_rr_idx] = rr;
            s_rr_idx = (s_rr_idx + 1) % RR_RING;
            if (s_rr_cnt < RR_RING) s_rr_cnt++;
            s_last_rr_ms = (float)rr * 1000.0f / (float)FS;
        }
    }

    if (s_rcount >= 3)
    {
        long cur_pos = pos - 1;                 /* r_cur = 中间那拍 */
        long r_cur  = s_rhist[cur_pos % RHIST];
        long r_prev = s_rhist[(cur_pos - 1) % RHIST];
        long r_new  = r;
        float pre  = (float)(r_cur - r_prev);
        float post = (float)(r_new - r_cur);
        float med  = median_rr(); if (med < 1.0f) med = 1.0f;
        long lo = cur_pos - RR_LOCAL_K; if (lo < 0) lo = 0;
        long cnt = cur_pos - lo; if (cnt < 1) cnt = 1;
        float local = (float)(r_cur - s_rhist[lo % RHIST]) / (float)cnt;
        if (local < 1.0f) local = 1.0f;

        s_pend_rr[0] = clip2_half(pre  / med);
        s_pend_rr[1] = clip2_half(post / med);
        s_pend_rr[2] = clip2_half(pre  / local);
        s_pend_r       = r_cur;
        s_pend_emit_at = r_cur + WIN_HALF;
        s_pend_active  = 1;
    }
}

/* ── 复位 ─────────────────────────────────────────────────────────── */
int arr_preproc_set_sample_rate(float hz)
{
    if (!isfinite(hz) || hz < 100.0f || hz > 2000.0f) return 0;
    if (fabsf(hz-s_input_hz)>0.01f) { s_input_hz=hz; arr_preproc_reset(); }
    return 1;
}

void arr_preproc_reset(void)
{
    memset(s_bq_z1, 0, sizeof(s_bq_z1));
    memset(s_bq_z2, 0, sizeof(s_bq_z2));
    memset(s_bp, 0, sizeof(s_bp));
    memset(s_sq_ring, 0, sizeof(s_sq_ring));
    memset(s_mwi_ring, 0, sizeof(s_mwi_ring));
    memset(s_raw, 0, sizeof(s_raw));
    s_rs_prev = 0.0f; s_rs_acc = 0.0; s_rs_have_prev = 0;
    s_sq_idx = 0; s_sq_sum = 0.0f;
    s_n360 = 0;
    s_learn = 1; s_learn_max = 0.0f; s_learn_sum = 0.0f; s_learn_cnt = 0;
    s_SPKI = 0.0f; s_NPKI = 0.0f; s_last_qrs = -REFR; s_rr_avg = (int)(0.8f * FS);
    s_mwi_prev = 0.0f; s_mwi_pprev = 0.0f; s_mwi_have = 0;
    s_rcount = 0;
    s_rr_idx = 0; s_rr_cnt = 0;
    s_last_rr_ms = 0.0f;
    s_pend_active = 0;
}

/* 处理一个 360Hz 采样,返回 1 表示产出心拍(填 s_beat_ecg / s_pend_rr) */
static int feed_360(float y, float ecg_out[WIN_LEN], float rr_out[ARR_PP_RR_DIM])
{
    long g = s_n360;
    s_raw[g % RAW_RING] = y;

    /* 带通(2 段 biquad,DF2T) */
    float x = y;
    for (int s = 0; s < 2; s++)
    {
        float in = x;
        float out = SOS[s][0] * in + s_bq_z1[s];
        s_bq_z1[s] = SOS[s][1] * in - SOS[s][3] * out + s_bq_z2[s];
        s_bq_z2[s] = SOS[s][2] * in - SOS[s][4] * out;
        x = out;
    }
    float bp = x;

    /* 5 点导数 */
    s_bp[4] = s_bp[3]; s_bp[3] = s_bp[2]; s_bp[2] = s_bp[1]; s_bp[1] = s_bp[0];
    s_bp[0] = bp;
    float der = (2.0f * s_bp[0] + s_bp[1] - s_bp[3] - 2.0f * s_bp[4]) * 0.125f;

    /* 平方 + 因果 MWI */
    float sq = der * der;
    s_sq_sum -= s_sq_ring[s_sq_idx];
    s_sq_ring[s_sq_idx] = sq;
    s_sq_sum += sq;
    s_sq_idx = (s_sq_idx + 1) % MWI_W;
    float mwi = s_sq_sum / (float)MWI_W;
    s_mwi_ring[g % MWI_RING] = mwi;

    /* 学习期:仅累计 MWI 统计,不检测 */
    if (s_learn)
    {
        if (mwi > s_learn_max) s_learn_max = mwi;
        s_learn_sum += mwi; s_learn_cnt++;
        if (s_learn_cnt >= LEARN_N)
        {
            s_SPKI  = s_learn_max * 0.25f;
            s_NPKI  = (s_learn_sum / (float)s_learn_cnt) * 0.5f;
            s_learn = 0;
            s_last_qrs = g;
        }
        s_mwi_pprev = s_mwi_prev; s_mwi_prev = mwi; s_mwi_have++;
        s_n360++;
        return 0;
    }

    /* MWI 局部极大检测(峰在 g-1) */
    if (s_mwi_have >= 2 && s_mwi_prev > s_mwi_pprev && s_mwi_prev >= mwi)
    {
        long  pidx = g - 1;
        float peak = s_mwi_prev;
        float thr1 = s_NPKI + 0.25f * (s_SPKI - s_NPKI);

        if (peak > thr1 && (pidx - s_last_qrs) > REFR)
        {
            register_r(pidx);
            s_SPKI = 0.125f * peak + 0.875f * s_SPKI;
            if (s_last_qrs > 0)
            {
                int rr = (int)(pidx - s_last_qrs);
                s_rr_avg = (int)(0.875f * s_rr_avg + 0.125f * rr);
            }
            s_last_qrs = pidx;
        }
        else
        {
            s_NPKI = 0.125f * peak + 0.875f * s_NPKI;
            /* searchback: 超 1.66*RR 无峰,半阈值回扫 MWI 环 */
            if (s_last_qrs > 0 && (pidx - s_last_qrs) > (long)(1.66f * s_rr_avg))
            {
                float thr2 = 0.5f * thr1;
                long a = s_last_qrs + REFR;
                long b = pidx;
                long oldest = g - MWI_RING + 1;
                if (a < oldest) a = oldest;
                long bestj = -1; float bestv = thr2;
                for (long k = a; k < b; k++)
                {
                    float v = s_mwi_ring[((k % MWI_RING) + MWI_RING) % MWI_RING];
                    if (v > bestv) { bestv = v; bestj = k; }
                }
                if (bestj >= 0)
                {
                    register_r(bestj);
                    s_SPKI = 0.25f * bestv + 0.75f * s_SPKI;
                    s_last_qrs = bestj;
                }
            }
        }
    }
    s_mwi_pprev = s_mwi_prev; s_mwi_prev = mwi; s_mwi_have++;

    /* 待产出心拍:等到 r_cur+128 已入环再切窗 */
    int produced = 0;
    if (s_pend_active && g >= s_pend_emit_at)
    {
        long start = s_pend_r - WIN_HALF;
        float mn = 1e30f, mx = -1e30f;
        for (int i = 0; i < WIN_LEN; i++)
        {
            long idx = start + i;
            float v = s_raw[((idx % RAW_RING) + RAW_RING) % RAW_RING];
            s_beat_ecg[i] = v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        float rng = mx - mn;
        if (rng > 1e-6f)
        {
            for (int i = 0; i < WIN_LEN; i++)
                ecg_out[i] = (s_beat_ecg[i] - mn) / rng;
            rr_out[0] = s_pend_rr[0];
            rr_out[1] = s_pend_rr[1];
            rr_out[2] = s_pend_rr[2];
            produced = 1;
        }
        s_pend_active = 0;
    }

    s_n360++;
    return produced;
}

/* ── 500Hz 线性重采样 → feed_360 ──────────────────────────────────── */
int arr_preproc_push_500(float sample_500,
                         float ecg_out[WIN_LEN],
                         float rr_out[ARR_PP_RR_DIM])
{
    const double ratio = (double)ARR_PP_FS / (double)s_input_hz;  /* 360/500 */
    int produced = 0;

    if (!s_rs_have_prev)
    {
        s_rs_prev = sample_500;
        s_rs_have_prev = 1;
        return 0;
    }

    s_rs_acc += ratio;
    while (s_rs_acc >= 1.0)
    {
        s_rs_acc -= 1.0;
        float f = 1.0f - (float)(s_rs_acc / ratio);                  /* 插值分数(prev→cur) */
        float y = s_rs_prev + (sample_500 - s_rs_prev) * f;
        produced |= feed_360(y, ecg_out, rr_out);
    }
    s_rs_prev = sample_500;
    return produced;
}

uint32_t arr_preproc_rpeak_count(void)
{
    return (uint32_t)s_rcount;
}

float arr_preproc_last_rr_ms(void)
{
    return s_last_rr_ms;
}
