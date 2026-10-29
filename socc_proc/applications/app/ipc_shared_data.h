/*
 * ipc_shared_data.h — M33/M55 双核 IPC 共享数据结构
 *
 * 两个工程各放一份完全相同的头文件：
 *   M33: socc_proc/applications/app/ipc_shared_data.h
 *   M55: Edgi_Talk_M55_LVGL/applications/ui/ipc_shared_data.h
 *
 * 内存布局：
 *   0x261C0000 ~ 0x261C07FF  mtb_ipc_shared_t + SRF 相关（BSP 占用）
 *   0x261C0800               sensor_ipc_data_t 起始地址（用户数据，~128B）
 *   0x261C0C00               ppg_waveform_ipc_t 起始地址（128×2 float 波形，1056B）
 *   0x261C1400               ecg_waveform_ipc_t 起始地址（512 float 环形缓冲，2080B）
 *
 * 标量只由 sensor_data.c 在 mutex 内发布，sequence 为奇数表示写入中，偶数表示完成。
 * touch_task 独立拥有 touch_*，发布标量时不得覆盖它们。
 * M55 必须失效 cache、复制快照并复查 sequence；新协议要求两核同时更新。
 */

#ifndef __IPC_SHARED_DATA_H__
#define __IPC_SHARED_DATA_H__

#include <stdint.h>


/* ------------------------------------------------------------------ */
/*  共享内存地址                                                        */
/* ------------------------------------------------------------------ */

#define IPC_SHARED_BASE      0x261C0000UL   /* m33_m55_shared 起始地址 */
#define IPC_USER_DATA_OFFSET 0x800          /* 保守偏移：跳过 BSP IPC 库占用区 */
#define IPC_USER_DATA_ADDR   (IPC_SHARED_BASE + IPC_USER_DATA_OFFSET)

/*
 * Magic cookie：M33 在写入第一份有效数据后才设此值。
 * M55 读 magic != IPC_MAGIC 时跳过，防止时序竞争导致读到 SRAM 残留垃圾。
 * 共享 SRAM 上电不清零，M55 启动比 M33 初始化早，必须用 magic 而非 sequence==0 判断。
 */
#define IPC_MAGIC            0x53444132UL   /* "SDA2" */

/* ------------------------------------------------------------------ */
/*  共享数据结构                                                        */
/* ------------------------------------------------------------------ */

/*
 * 全部使用 32 位宽类型，避免 M33/M55 编译器 padding 差异。
 * 32 字节对齐满足 M55 D-Cache line 对齐要求。
 */
typedef struct __attribute__((aligned(32)))
{
    /* --- PPG (MAX30102) --- */
    int32_t  heart_rate;      /* 心率，bpm */
    int32_t  spo2;            /* 血氧饱和度，% */
    float    blood_flow;      /* 血流速度，a.u. */
    int32_t  ppg_valid;       /* 1 = 本轮数据有效 */

    /* --- 红外体温 (MLX90614 TOBJ1) --- */
    float    t_ir;            /* 物体温度，degC */
    int32_t  mlx_valid;

    /* 外接 SHT30 环境数据；板载 AHT20 只用于板温监控。 */
    float    sht30_temp;      /* 环境温度，degC */
    float    sht30_humi;      /* 环境湿度，%RH */
    int32_t  sht30_valid;

    /* --- 板载温湿度 (AHT20，仅参考，有热漂移 ~+7°C) --- */
    float    t_board;         /* 板载温度，degC */
    float    h_board;         /* 板载湿度，%RH */
    int32_t  aht_valid;

    /* BMR 保留字段；人口学资料及训练单位契约未建立，valid 恒 0。 */
    float    bmr;             /* 基础代谢率，kcal/h */
    int32_t  bmr_valid;

    /* --- 血糖 (P2 NPU 推理写回，当前为 0) --- */
    float    glucose;         /* 血糖浓度，mmol/L */
    int32_t  glucose_valid;   /* 0 = P2 前不可用 */

    /* --- 应激 (Grove GSR，经 mux CH4 -> ADS1015 AIN1) ---
     * gsr_stress_index 是相对基线的 0-100 评分，非标定过的绝对电导单位。 */
    float    gsr_stress_index; /* 应激指数，0-100 */
    int32_t  gsr_valid;        /* 0 = 学习期未结束/未接入 */

    /* PAT 保留字段；缺少采样时间同步，valid 恒 0。 */
    float    pat_ms;           /* PAT，ms */
    int32_t  pat_valid;        /* 0 = 未配对成功/超出生理合理范围 */

    /* --- 触摸坐标 (touch_task 写，30ms 周期) --- */
    int32_t  touch_x;         /* 触摸 X 坐标，-1 = 无触摸 */
    int32_t  touch_y;         /* 触摸 Y 坐标，-1 = 无触摸 */
    int32_t  touch_pressed;   /* 1 = 按下，0 = 松开 */
    uint32_t touch_seq;       /* 触摸独立序号（30ms 递增，不影响传感器 sequence） */

    /* --- 同步字段 --- */
    uint32_t magic;           /* IPC_MAGIC：M33 写入有效数据后设置，M55 读此判断是否可信 */
    uint32_t sequence;        /* 奇数写入中，偶数完成；M55 复制后复核 */
    uint32_t tick;            /* rt_tick_get()，写入时间戳 */

    /* 测量会话状态，取值见 ppg_session.h。 */
    int32_t  ppg_session_state;    /* 0=idle 1=wait 2=settle 3=measure 4=done 5=failed */
    int32_t  ppg_session_progress; /* 测量进度 0-100 */
    /*
     * 最后一次被门控拒绝的原因，取值见 algo/ppg_sqi.h 的 ppg_gate_result_t。
     *
     * 为什么要跨核传：PI_MAX 收紧到 10 后(拦运动伪迹，见 ppg_sqi.h 的证据)，
     * 拒绝率必然上升。若 UI 只会说"测量失败 信号不稳"，用户不知道该怎么办；
     * 而"手指请勿移动"是可执行的。失败提示的价值全在于**能指导下一步动作**。
     */
    int32_t  ppg_reject_reason;

    /* 注：HRV / 呼吸率**不走 IPC** —— M55 侧 arrhythmia_task 本就有自己的 R 峰
     * 检测器(arrhythmia_preproc)，health_metrics.c 直接吃它的 R-R 就地算完，
     * 没有跨核传输的必要。别再往这里加 hrv_* 字段。 */

    uint32_t ppg_measurement_id; /* 成功测量递增；与波形关联 */
    uint32_t ppg_tick;           /* M33 完成时间，仅同一核时间域内比较 */
    uint32_t ppg_request_seq;    /* M33 已受理的请求，与撤销有效位一起发布 */
} sensor_ipc_data_t;

/* ------------------------------------------------------------------ */
/*  访问接口                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief 获取共享内存中 sensor_ipc_data_t 的指针
 *
 * M33 只由 sensor_data.c 在锁内按奇偶 sequence 发布；M55 需复制后复核。
 */
static inline volatile sensor_ipc_data_t *ipc_get_data_ptr(void)
{
    return (volatile sensor_ipc_data_t *)IPC_USER_DATA_ADDR;
}

/* ------------------------------------------------------------------ */
/*  PPG 波形 IPC（A6：128×2 float 下采样波形，独立于标量传感器数据）      */
/* ------------------------------------------------------------------ */

/*
 * 独立地址区域，不塞进 sensor_ipc_data_t：
 *   - 避免主结构膨胀 1KB（ui_data.c 500ms 轮询不需要 invalidate 波形）
 *   - 波形有独立的 sequence 计数器（5s 周期，跟随 ppg_task）
 *   - glucose_task 按需读取波形，与 UI 轮询解耦
 */
#define IPC_PPG_WAVEFORM_ADDR  (IPC_SHARED_BASE + 0x0C00)
#define IPC_PPG_MAGIC          0x50504732UL   /* "PPG2" */

#define PPG_IPC_LEN      128   /* 下采样后每通道点数 */
#define PPG_IPC_CHANNELS  2    /* [0]=RED, [1]=IR */

typedef struct __attribute__((aligned(32)))
{
    float    data[PPG_IPC_LEN][PPG_IPC_CHANNELS]; /* 128×2 float = 1024B, [0,1] 归一化 */
    uint32_t magic;       /* IPC_PPG_MAGIC：M33 写入有效波形后设置 */
    uint32_t sequence;    /* M33 ppg_task 每轮写后递增 */
    uint32_t tick;        /* rt_tick_get()，波形采集完成时间 */
    uint32_t measurement_id;
    uint32_t _pad[4];     /* 保持 1056B */
} ppg_waveform_ipc_t;

static inline volatile ppg_waveform_ipc_t *ipc_get_waveform_ptr(void)
{
    return (volatile ppg_waveform_ipc_t *)IPC_PPG_WAVEFORM_ADDR;
}

/* ------------------------------------------------------------------ */
/*  ECG 波形 IPC（AD8232 经 ADS1015 连续采集，环形缓冲，供心律不齐 NPU）  */
/* ------------------------------------------------------------------ */

/*
 * 跟 PPG 波形不同：心律不齐前处理(R 峰检测)需要连续流，不是"每 5s 一批"，
 * 所以用环形缓冲 + 单调递增 write_seq，M55 侧按自己的 last_read_seq 追赶读取，
 * 而不是整块覆盖。地址选在 PPG 波形区(0x0C00~0x1020)之后，留足间距。
 */
#define IPC_ECG_WAVEFORM_ADDR  (IPC_SHARED_BASE + 0x1400)
#define IPC_ECG_MAGIC          0x45434732UL   /* "ECG2" */

#define ECG_IPC_RING_LEN  512   /* 循环缓冲长度，@500Hz 约 1s 余量，足够 M55 轮询延迟 */

typedef struct __attribute__((aligned(32)))
{
    float    ring[ECG_IPC_RING_LEN];  /* 循环缓冲，单位 mV（ADS1015 原始码近似 mV） */
    uint32_t write_seq;       /* M33 ecg_task 单调递增：总共写入的样本数（含环绕） */
    uint32_t magic;           /* IPC_ECG_MAGIC：首次写入后设置 */
    uint32_t sample_rate_hz;  /* M33 实测的实际采样率，供 M55 侧核对/调试 */
    uint32_t tick;            /* rt_tick_get()，最近一次写入时间戳 */
    uint32_t stream_epoch;    /* 通信失败/采样间断时递增，消费者重置连续窗口 */
    uint32_t _pad[3];         /* 保持 2080B */
} ecg_waveform_ipc_t;

static inline volatile ecg_waveform_ipc_t *ipc_get_ecg_ptr(void)
{
    return (volatile ecg_waveform_ipc_t *)IPC_ECG_WAVEFORM_ADDR;
}

/* ------------------------------------------------------------------ */
/*  命令 IPC（★反向：M55 写 → M33 读，全项目唯一反方向通道）           */
/* ------------------------------------------------------------------ */

/*
 * 用途：M55 屏上的"开始测量"按钮把用户意图传给 M33 的采集核。
 *
 * 方向与其余所有 IPC 相反（其余是 M33 写 M55 读），所以 D-Cache 处理也相反：
 *   - M55 写：改字段后 SCB_CleanDCache_by_Addr()（把 M55 cache 刷回 SRAM），
 *             再递增 seq。CM55 有 D-Cache，不 clean 则 M33 读到旧值。
 *   - M33 读：CM33 无 D-Cache，DSB 后直接读即可。
 *
 * 独立地址 + 独立 magic：绝不塞进 sensor_ipc_data_t（那个是 M33 单向写，
 * 混入 M55 写字段会让两核的 cache 操作互相打架）。
 *
 * 语义：seq 单调递增，M33 用 last_cmd_seq 比对检测"新命令"（同 touch_seq 模式），
 * 不需要 M33 回写清零 —— 反向清零又要 M33 写、M55 读，徒增一条反反向通道。
 */
#define IPC_CMD_ADDR   (IPC_SHARED_BASE + 0x2000)
#define IPC_CMD_MAGIC  0x434D4432UL   /* "CMD2": 支持取消与请求确认，须两核成套更新 */

/* 命令码 */
#define PPG_CMD_NONE           0
#define PPG_CMD_START_MEASURE  1   /* 用户按下"开始测量" */
#define PPG_CMD_CANCEL_MEASURE 2   /* 取消并撤销本次结果，M33受理后确认序号 */

typedef struct __attribute__((aligned(32)))
{
    uint32_t magic;     /* IPC_CMD_MAGIC：M55 首次写入后设置 */
    uint32_t seq;       /* M55 每次按钮递增，M33 比对检测新命令 */
    int32_t  cmd;       /* PPG_CMD_*，与 seq 配对 */
    uint32_t _pad[5];   /* 填充到 32B，满足 D-Cache line 对齐 */
} ppg_cmd_ipc_t;

static inline volatile ppg_cmd_ipc_t *ipc_get_cmd_ptr(void)
{
    return (volatile ppg_cmd_ipc_t *)IPC_CMD_ADDR;
}

#endif /* __IPC_SHARED_DATA_H__ */
