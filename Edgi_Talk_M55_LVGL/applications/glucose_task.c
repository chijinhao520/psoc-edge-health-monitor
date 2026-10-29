/*
 * glucose_task.c — M55 血糖推理任务
 *
 * 线程 "glucose"，priority=20，stack=4096（堆分配，不占 DTCM BSS）。
 * 每 5s 从 IPC 读取 PPG 波形 + 标量特征 → 推理 → 结果存本地 static。
 *
 * 推理分级：L1 = TFLite Micro + Ethos-U55 NPU（主路径，已上板）；
 * L1 不可用时落 L3 占位伪公式 —— 仅为通路验证，输出无医学意义，
 * 因此 L3 产物 valid=0（UI 显示 "--"），只进日志与调试字段。
 */

#include <rtthread.h>
#include <board.h>
#include "glucose_task.h"
#include "glucose_inference.h"  /* S1.A: 调 init 验证 rt_kprintf 路径 */
#include "ui/ipc_shared_data.h"
#include "ui/ipc_snapshot.h"
#include <math.h>

#define DBG_TAG "glucose"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* ------------------------------------------------------------------ */
/*  配置                                                                */
/* ------------------------------------------------------------------ */

#define GLUCOSE_THREAD_STACK  4096
#define GLUCOSE_THREAD_PRIO   20
#define GLUCOSE_INIT_DELAY_MS 15000   /* 初始延迟：等传感器/WiFi 稳定 */
#define GLUCOSE_PERIOD_MS     5000    /* 推理周期，匹配 PPG 采集 */

/* ------------------------------------------------------------------ */
/*  推理结果（M55 本地，同核读写无需 IPC）                               */
/* ------------------------------------------------------------------ */

static glucose_result_t s_result = { .glucose = 0.0f, .valid = 0 };

void glucose_copy_result(glucose_result_t *out)
{
    rt_base_t level=rt_hw_interrupt_disable();
    if (out) *out=s_result;
    rt_hw_interrupt_enable(level);
}

const glucose_result_t *glucose_get_result(void)
{
    return &s_result;
}

/* ------------------------------------------------------------------ */
/*  占位推理（P2 替换为 TFLite Micro）                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief 占位推理函数
 *
 * 输入：128×2 归一化波形 + 标量特征
 * 输出：伪血糖值 mmol/L（仅为端到端通路验证，无医学意义）
 *
 * P2 阶段此函数体替换为：
 *   1. 量化输入 float→int8 (TFLite input tensor)
 *   2. tflite::MicroInterpreter::Invoke()
 *   3. 反量化 output tensor→float
 */
static float glucose_inference_placeholder(
    const float waveform[PPG_IPC_LEN][PPG_IPC_CHANNELS],
    int32_t hr, int32_t spo2, float bv,
    float bmr, float t_ir, float t_env, float h_env)
{
    /* 波形特征：RED/IR 通道均值 */
    float red_sum = 0.0f, ir_sum = 0.0f;
    for (int i = 0; i < PPG_IPC_LEN; i++)
    {
        red_sum += waveform[i][0];
        ir_sum  += waveform[i][1];
    }
    float red_mean = red_sum / PPG_IPC_LEN;
    float ir_mean  = ir_sum  / PPG_IPC_LEN;

    /* 伪血糖 = 基线 + 波形微扰 + 标量影响 */
    float glucose = 5.0f
                  + (red_mean - 0.5f) * 2.0f
                  + (ir_mean  - 0.5f) * 0.5f
                  + (float)hr * 0.005f
                  + (float)spo2 * (-0.01f)
                  + bmr * 0.002f;

    (void)bv; (void)t_ir; (void)t_env; (void)h_env;

    /* 钳位到生理范围 [2.0, 20.0] mmol/L */
    if (glucose < 2.0f)  glucose = 2.0f;
    if (glucose > 20.0f) glucose = 20.0f;

    return glucose;
}

/* ------------------------------------------------------------------ */
/*  线程入口                                                            */
/* ------------------------------------------------------------------ */

static void glucose_thread_entry(void *param)
{
    RT_UNUSED(param);

    uint32_t last_wf_seq  = 0;
    uint32_t last_scalar_seq = 0;
    rt_tick_t last_scalar_change = rt_tick_get();
    float    local_wf[PPG_IPC_LEN][PPG_IPC_CHANNELS];

    LOG_I("glucose task started, waiting %d ms for sensors+NPU",
          GLUCOSE_INIT_DELAY_MS);
    rt_thread_mdelay(GLUCOSE_INIT_DELAY_MS);

    /* S3: TFLite Micro + Ethos-U55 NPU 推理引擎初始化 */
    int infer_ret = glucose_inference_init();
    if (infer_ret == 0)
    {
        LOG_I("NPU inference engine ready, arena_used=%u bytes",
              (unsigned)glucose_inference_arena_used());
    }
    else
    {
        LOG_W("NPU inference init failed (ret=%d), using placeholder fallback",
              infer_ret);
    }

    while (1)
    {
        rt_thread_mdelay(GLUCOSE_PERIOD_MS);

        sensor_ipc_data_t scalar;
        static ppg_waveform_ipc_t wave;
        int scalar_ok=ipc_sensor_snapshot(&scalar);
        rt_tick_t now=rt_tick_get();
        if (scalar_ok && scalar.sequence!=last_scalar_seq) {
            last_scalar_seq=scalar.sequence; last_scalar_change=now;
        }
        if (!scalar_ok || now-last_scalar_change>rt_tick_from_millisecond(3000) || !scalar.ppg_valid ||
            !scalar.bmr_valid || !ipc_wave_snapshot(&wave) ||
            wave.measurement_id!=scalar.ppg_measurement_id)
        {
            rt_base_t level=rt_hw_interrupt_disable();
            s_result.valid=0;
            rt_hw_interrupt_enable(level);
            continue;
        }
        if (wave.sequence==last_wf_seq) continue;
        last_wf_seq=wave.sequence;
        rt_memcpy(local_wf,wave.data,sizeof(local_wf));
        int32_t hr=scalar.heart_rate, spo2=scalar.spo2;
        float bv=0, bmr=scalar.bmr, t_ir=scalar.t_ir;
        float t_env=scalar.sht30_temp, h_env=scalar.sht30_humi;

        /* ---- 3. 推理(L1 NPU 优先,失败回退 L3 placeholder) ---- */
        float glucose;
        const char *src;
        int invoke_ret = -1;

        if (glucose_inference_is_ready())
        {
            /*
             * 7 维标量打包。顺序以训练脚本为准：
             *   data_must.py:26  X_scal (N,7) = [HR, SpO2, BV, t_ir, t_env, h_env, BMR]
             * 此前这里是 [HR,SpO2,BV,**BMR**,t_ir,t_env,h_env]，后 4 维全部串位
             * （注释却自称"顺序与训练一致"）→ BMR 被当成 t_ir 喂进去。
             *
             * ★ 只有 HR / BMR 喂真实值，其余 5 维必须喂训练时的常量。
             *
             * 反直觉但必须：查 must.npz 实测 201 个训练样本的标量分布 ——
             *   HR    50~116.22 (std 15.70)  mask=100%  ← 真实
             *   SpO2  98.00~98.00 (std 0)    mask=0%    ← 全程常量
             *   BV    1.00~1.00   (std 0)    mask=0%
             *   t_ir  36.50~36.50 (std 0)    mask=0%
             *   t_env 25.00~25.00 (std 0)    mask=0%
             *   h_env 55.00~55.00 (std 0)    mask=0%
             *   BMR   46.10~84.95 (std 9.99) mask=100%  ← 真实
             * 模型只见过 HR/BMR 变化，对另外 5 维学不到任何映射（输入恒定→贡献
             * 被 bias 吸收）。喂真实测量值 = 让模型在从未见过的输入区域外推，
             * 输出不可预测 —— 那不是"用上了真实数据"，是制造分布偏移。
             *
             * 所以这里刻意丢弃真实的 spo2/t_ir/t_env/h_env，不是忘了接。
             * 待数据集补齐这些维的 ground truth 重训后，再改回喂真实值。
             * 常量取值 = 训练脚本的 SCAL_DEFAULTS（data_must.py:72 / data_kaggle.py:62）。
             */
            float scal[7] = {
                (float)hr,   /* 真实：训练侧 estimate_hr(seg500) 现算 */
                98.0f,       /* SCAL_DEFAULTS["SpO2"] */
                1.0f,        /* SCAL_DEFAULTS["BV"]  —— 与 blood_flow 死值 14.02 无关 */
                36.5f,       /* SCAL_DEFAULTS["t_ir"] */
                25.0f,       /* SCAL_DEFAULTS["t_env"] */
                55.0f,       /* SCAL_DEFAULTS["h_env"] */
                bmr          /* 真实：训练侧 Mifflin-St Jeor 由 Age/Gender/H/W 算 */
            };
            /* 归一化由 glucose_inference_invoke 内部照训练契约做，此处送原始物理量 */
            invoke_ret = glucose_inference_invoke(local_wf, scal, &glucose);
        }

        int src_id, valid;
        if (invoke_ret == 0)
        {
            src    = "NPU";
            src_id = GLU_SRC_NPU;
            valid  = 1;
        }
        else
        {
            glucose = glucose_inference_placeholder(
                local_wf, hr, spo2, bv, bmr, t_ir, t_env, h_env);
            src    = "PLACEHOLDER";
            src_id = GLU_SRC_PLACEHOLDER;
            /* 占位公式自认无医学意义 → 不可作为有效值展示/评分。
             * 数值仍写入 glucose 字段：日志与调试可见，通路验证不受影响 */
            valid  = 0;
        }

        /* 结果与输入会话再次核对，并在短临界区内整体发布。 */
        sensor_ipc_data_t current;
        if (!ipc_sensor_snapshot(&current) || !current.ppg_valid || !current.bmr_valid ||
            current.ppg_measurement_id!=scalar.ppg_measurement_id ||
            current.ppg_request_seq!=scalar.ppg_request_seq)
            valid=0;
        valid=valid && isfinite(glucose) && glucose>=1.0f && glucose<=35.0f;
        rt_base_t level=rt_hw_interrupt_disable();
        s_result.valid   = 0;
        s_result.glucose = glucose;
        s_result.tick    = rt_tick_get();
        s_result.wf_seq  = last_wf_seq;
        s_result.measurement_id=scalar.ppg_measurement_id;
        s_result.source  = src_id;
        if (valid)
            s_result.valid = 1;
        rt_hw_interrupt_enable(level);

        LOG_I("glucose=%d.%02d mmol/L [%s] (wf_seq=%u, hr=%d, spo2=%d)",
              (int)glucose,
              (int)((glucose - (int)glucose) * 100.0f + 0.5f),
              src, last_wf_seq, hr, spo2);
    }
}

/* ------------------------------------------------------------------ */
/*  初始化                                                              */
/* ------------------------------------------------------------------ */

static int glucose_task_init(void)
{
    rt_thread_t tid = rt_thread_create(
        "glucose",
        glucose_thread_entry,
        RT_NULL,
        GLUCOSE_THREAD_STACK,
        GLUCOSE_THREAD_PRIO,
        10);

    if (tid == RT_NULL)
    {
        LOG_E("glucose thread create failed");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    LOG_I("glucose task created (prio=%d, stack=%d)",
          GLUCOSE_THREAD_PRIO, GLUCOSE_THREAD_STACK);
    return RT_EOK;
}
INIT_APP_EXPORT(glucose_task_init);
