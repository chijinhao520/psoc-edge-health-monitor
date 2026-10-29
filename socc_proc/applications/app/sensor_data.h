/*
 * 全局传感器数据汇聚结构体
 *
 * 三路采集线程（ppg_task / sht30_task / mlx90614_task）各自写入，
 * BMR 计算模块和上层应用通过 sensor_data_get() 读取快照。
 *
 * 线程安全：内部持有一把 mutex，所有写入和读取均在 mutex 保护内完成。
 */

#ifndef __SENSOR_DATA_H__
#define __SENSOR_DATA_H__

#include <rtthread.h>

/* ------------------------------------------------------------------ */
/*  共享数据结构                                                        */
/* ------------------------------------------------------------------ */

struct sensor_data
{
    uint32_t ppg_measurement_id;
    uint32_t ppg_tick;
    uint32_t ppg_request_seq; /* 已受理的开始测量请求 */
    int32_t ppg_session_state, ppg_session_progress, ppg_reject_reason;
    /* --- PPG (MAX30102) --- */
    int32_t  heart_rate;    /* 心率，bpm */
    int32_t  spo2;          /* 血氧饱和度，% */
    float    blood_flow;    /* 血流速度，a.u. */
    rt_bool_t ppg_valid;    /* RT_TRUE = 本轮数据有效 */

    /* --- 温湿度 (SHT30) ---
     * 模块阶段：外接远离板子，测真实环境温湿度（BMR 的 t_env / h_env）
     * 集成板阶段：贴指腹，测接触温度和局部湿度（BMR 的 t_finger / h_env） */
    float    sht30_temp;    /* SHT30 温度，degC */
    float    sht30_humi;    /* SHT30 湿度，%RH */
    rt_bool_t sht30_valid;

    /* --- 红外测温 (MLX90614) --- */
    float    t_ir;          /* 物体（额头/手腕）红外温度，degC */
    rt_bool_t mlx_valid;

    /* --- 板载温湿度 (AHT20) ---
     * 注意：受 PCB 发热影响，实测热漂移 ~+7°C，不适合作为环境温度源。
     * 仅用于板温监控和热漂移参考，不参与 BMR 计算。 */
    float    t_board;       /* 板载温度，degC（受芯片发热干扰） */
    float    h_board;       /* 板载湿度，%RH */
    rt_bool_t aht20_valid;

    /* BMR 保留接口；未建立人口学资料/训练单位契约，目前始终无效。 */
    float    bmr;           /* 基础代谢率，kcal/h */
    rt_bool_t bmr_valid;

    /* --- 应激 (Grove GSR，经 ADS1015 AIN1) ---
     * gsr_stress_index 是相对基线的 0-100 评分，不是标定过的绝对电导单位
     * （无校准硬件，风格同 PPG-SQI）；gsr_valid 在学习期结束前为 RT_FALSE。 */
    float    gsr_raw_mv;        /* 原始 ADC 读数，mV，调试用 */
    float    gsr_stress_index;  /* 应激指数，0-100 */
    rt_bool_t gsr_valid;

    /* PAT 保留接口；未建立两路采样时间同步，目前始终无效。 */
    float    pat_ms;
    rt_bool_t pat_valid;
};

/* ------------------------------------------------------------------ */
/*  API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化共享数据结构（创建 mutex）
 *
 * 由 INIT_APP_EXPORT 自动调用，优先级最高（在三个任务之前），
 * 保证 mutex 在任何写入发生前已就绪。
 */
int sensor_data_init(void);
void sensor_data_refresh(void);
void sensor_data_begin_measurement(uint32_t request_seq);
void sensor_data_update_session(int state, int progress, int reject);

/**
 * @brief PPG 任务写入心率、血氧、血流速度
 */
void sensor_data_update_ppg(int32_t hr, int32_t spo2, float bv, rt_bool_t valid);

/**
 * @brief SHT30 任务写入温湿度（模块阶段=环境，集成板=手指）
 */
void sensor_data_update_sht30(float sht30_temp, float sht30_humi);

/**
 * @brief MLX90614 任务写入红外体温（仅 TOBJ1）
 */
void sensor_data_update_mlx90614(float t_ir);

/**
 * @brief AHT20 任务写入板载温湿度（仅监控，不参与 BMR 计算）
 */
void sensor_data_update_aht20(float t_board, float h_board);

/**
 * @brief BMR 任务写入计算结果
 */
void sensor_data_update_bmr(float bmr);

/**
 * @brief GSR 任务写入原始读数 + 应激指数
 *
 * @param raw_mv        原始 ADC 读数，mV
 * @param stress_index  应激指数，0-100（相对基线评分）
 * @param valid         RT_TRUE = 已过学习期，指数可信
 */
void sensor_data_update_gsr(float raw_mv, float stress_index, rt_bool_t valid);

/**
 * @brief pat_calc 写入 PAT 计算结果
 *
 * @param pat_ms  PAT，ms（已做检测延迟修正）；invalid 时传 0
 * @param valid   RT_TRUE = 本次配对在生理合理范围内
 */
void sensor_data_update_pat(float pat_ms, rt_bool_t valid);

/**
 * @brief 读取当前数据快照（线程安全拷贝）
 *
 * @param out  调用方提供的结构体，函数将最新数据拷贝到此处
 */
void sensor_data_get(struct sensor_data *out);

#endif /* __SENSOR_DATA_H__ */
