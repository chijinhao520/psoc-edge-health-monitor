/*
 * hr_sil.c — 心率算法 PC 侧 SIL（软件在环）harness
 *
 * 目的：在 PC 上用**已知真值**的合成 PPG 波形驱动固件里那份心率算法，
 * 使心率准确性可以脱离"真人佩戴 + 参考设备"来回归验证。
 *
 * ★ 直接编译 applications/algo/ppg_process.c 本体，不复制算法 ——
 *   复制一份就会随生产代码演进而失真，测的必须是真正烧进板子的那份。
 *
 * 当前契约：ppg_process.c 直接按 HR=60*fs/峰间隔输出 bpm，不在调用侧二次修正。
 * fs 是传感器连续样本时钟；FIFO 轮询耗时不代表采样率。
 *
 * 用法（见同目录 run.ps1，会扫描多个峰距取值 × 多档采样率）：
 *   cl /nologo /I../../applications/algo -DHR_MIN_PEAK_DIST_MS=340 \
 *      hr_sil.c ../../applications/algo/ppg_process.c /Fe:hr_sil.exe
 *   hr_sil.exe [fs]      fs 缺省 88（板上实测值），传 100 可看标称情形
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

#include "ppg_process.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ===================== 合成 PPG 波形 =====================
 *
 * 真实 PPG 每一拍有两个隆起：收缩峰 + 其后约 250ms 的重搏波(dicrotic notch)。
 * 重搏波正是"峰距太小 → 每拍数两次 → 心率翻倍"的元凶，所以合成波形**必须**
 * 含它，否则这个 harness 会漏掉真实存在的失效模式。
 *
 * 幅度参数对齐板上实测量级：DC≈50000，PI(灌注指数)≈4.8%。
 */
#define PPG_DC        50000.0
#define PPG_PI        0.048               /* AC/DC，板上实测 4.80% */
#define DICROTIC_DLY  0.25                /* 重搏波滞后收缩峰(s) */
#define DICROTIC_AMP  0.40                /* 重搏波相对收缩峰幅度 */
#define SYS_WIDTH     0.055               /* 收缩峰高斯宽度(s) */
#define DIC_WIDTH     0.075               /* 重搏波高斯宽度(s) */

static double gauss_pulse(double t, double center, double width)
{
    double z = (t - center) / width;
    return exp(-z * z);
}

/*
 * 生成 n 点合成 PPG。
 *   bpm : 真值心率
 *   fs  : 采样率(Hz) —— 与算法内部假设的 FS 解耦，这正是要验证的一环
 *   noise_amp : 相对 AC 的白噪声幅度(0=无噪声，确定性)
 */
static void synth_ppg(uint32_t *ir, uint32_t *red, int n,
                      double bpm, double fs, double noise_amp)
{
    double period = 60.0 / bpm;           /* 一拍时长(s) */
    double ac_amp = PPG_DC * PPG_PI;
    double total_t = n / fs;
    int    n_beats = (int)(total_t / period) + 3;
    int    i, k;

    for (i = 0; i < n; i++)
    {
        double t = i / fs;
        double ac = 0.0;
        double noise;

        for (k = -1; k < n_beats; k++)
        {
            double tb = k * period;
            ac += gauss_pulse(t, tb, SYS_WIDTH);
            ac += DICROTIC_AMP * gauss_pulse(t, tb + DICROTIC_DLY, DIC_WIDTH);
        }

        /* 确定性伪噪声（不用 rand()，保证跨平台/跨次运行结果一致） */
        noise = noise_amp * sin(i * 12.9898) * 43758.5453;
        noise = noise - floor(noise);
        noise = (noise - 0.5) * 2.0 * noise_amp;

        ir[i]  = (uint32_t)(PPG_DC + ac_amp * (ac + noise));
        /* RED 通道：本 harness 不校验 SpO2，给一个同形态、幅度略低的信号即可 */
        red[i] = (uint32_t)(PPG_DC * 0.92 + ac_amp * 0.85 * (ac + noise));
    }
}



/* ===================== 主流程 ===================== */

static uint32_t ir_buf[BUFFER_SIZE];
static uint32_t red_buf[BUFFER_SIZE];

int main(int argc, char **argv)
{
    /* 覆盖静息体检量程；项目 HR 生理闸为 40~150 */
    static const double truth_bpm[] = { 45, 55, 65, 75, 90, 110, 130, 145 };
    const int  n_cases   = (int)(sizeof(truth_bpm) / sizeof(truth_bpm[0]));
    const int  tol       = 1;             /* 合成波形回归容差，非实机精度声明 */
    double     fs        = 100.0;         /* 传感器样本时钟，可由 argv[1] 覆盖 */
    int        n_fail    = 0;
    int        i;

    if (argc > 1)
        fs = atof(argv[1]);

    printf("=== HR SIL: peak_dist=%dms (=%d pts @%.0fHz)  fs=%.0fHz ===\n",
           HR_MIN_PEAK_DIST_MS,
           (int)(fs * HR_MIN_PEAK_DIST_MS / 1000.0), fs, fs);
    printf("%6s %10s %10s %8s  %s\n",
           "truth", "algorithm", "bpm", "err", "verdict");

    for (i = 0; i < n_cases; i++)
    {
        int32_t spo2, hr_maxim, hr_corr, err;
        int8_t  spo2_valid, hr_valid;

        synth_ppg(ir_buf, red_buf, BUFFER_SIZE, truth_bpm[i], fs, 0.0);

        /* 传入实测 fs：算法据此把 HR_MIN_PEAK_DIST_MS 折算成采样点 */
        maxim_heart_rate_and_oxygen_saturation(ir_buf, BUFFER_SIZE, red_buf,
                                               &spo2, &spo2_valid,
                                               &hr_maxim, &hr_valid,
                                               (uint32_t)(fs + 0.5));

        if (!hr_valid)
        {
            /* 控制台输出一律 ASCII：Windows 控制台常为 GBK，UTF-8 中文会乱码 */
            printf("%6.0f %10s %10s %8s  INVALID (too few peaks)\n",
                   truth_bpm[i], "-", "-", "-");
            n_fail++;
            continue;
        }

        hr_corr = hr_maxim; /* 直接验证生产算法输出，不在测试中修正结果 */
        err     = hr_corr - (int32_t)truth_bpm[i];

        printf("%6.0f %10d %10d %+8d  %s\n",
               truth_bpm[i], hr_maxim, hr_corr, err,
               (abs(err) <= tol) ? "ok" : "**OUT OF TOL**");

        if (abs(err) > tol)
            n_fail++;
    }

    printf("--- out-of-tol %d/%d (tolerance +/-%dbpm) ---\n\n", n_fail, n_cases, tol);
    return (n_fail > 0) ? 1 : 0;
}
