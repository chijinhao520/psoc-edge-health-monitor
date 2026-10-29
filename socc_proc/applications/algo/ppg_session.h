/*
 * PPG 测量会话状态机 — 用户发起/取消 + 稳定期 + 多帧聚合
 *
 * 为什么需要它（2026-07-16 实测依据）：
 *
 * 1. **手指刚贴上时基线在漂移**，不能直接用。实测手指贴合瞬间 DC 已达 4 万
 *    (说明在位)，但 PI 高达 48%、CV=1.02、IBIstd=570ms，maxim 算出 hr=115/333，
 *    全是基线未稳导致的伪迹。必须丢弃开头若干轮。
 *
 * 2. **单帧结果不稳**。PPG 有经典的重搏波误检问题，个别帧心率会翻倍
 *    (实测 SQI 侧峰检测 n=4≈48bpm 而 maxim 报 153)。取多帧中位数可以
 *    直接把这类离群帧压掉 —— 中位数比均值抗离群。
 *
 * 3. **需要明确的测量语义**。用户要知道"什么时候在测、测完没有"，
 *    而不是对着一个一直跳的数字发呆。
 *
 * 用户按开始后，DC辅助判定手指贴合。取消与重新开始均经M55→M33请求确认；
 * 任务使用真实tick限制总时长，不能让坏帧/换源造成无限等待。
 */

#ifndef __PPG_SESSION_H__
#define __PPG_SESSION_H__

#include <rtthread.h>
#include "ppg_sqi.h"

/* ======================== 会话参数 ======================== */

/*
 * 稳定期轮数。每轮 = 500 点 @100Hz ≈ 5s。
 * 取 1 轮：实测基线漂移主要发生在贴合后的头几秒，丢 1 轮(5s)足够，
 * 丢太多会让用户等得不耐烦(总时长 = 5s 稳定 + 15s 测量 = 20s)。
 */
#define PPG_SESSION_SETTLE_ROUNDS   1

/*
 * 测量期需要收集的**过闸**帧数。取 3 帧(≈15s)：
 * - 中位数至少要 3 个样本才有意义
 * - 3 帧 15s 与"3 分钟出体检报告"的产品节奏相容
 * 注意是"过闸帧"计数，被门控拒绝的帧不计入，所以信号差时会自动延长。
 */
#define PPG_SESSION_MEASURE_FRAMES  3

/* 测量期允许的连续坏帧数。超过说明用户手指在动/贴合不良，回退重来，
 * 避免在一个坏姿势上无限等待 */
#define PPG_SESSION_MAX_BAD_FRAMES  6

/* ======================== 状态 ======================== */

typedef enum
{
    PPG_SESSION_IDLE = 0,   /* 待命，等用户按"开始测量" */
    PPG_SESSION_WAIT_FINGER,/* 已按开始，但手指还没贴上，提示放手指 */
    PPG_SESSION_SETTLING,   /* 手指已贴合，等基线稳定（丢弃期） */
    PPG_SESSION_MEASURING,  /* 测量中，累积过闸帧 */
    PPG_SESSION_DONE,       /* 已出结果，手指移开前保持 */
    PPG_SESSION_FAILED,     /* 本次测量失败，停留数轮让用户看见再回 IDLE */
    PPG_SESSION_CANCELLED,  /* 用户主动取消；保持到下次开始 */
} ppg_session_state_t;

/* 失败态停留轮数：ABORT 后若直接回 IDLE，UI 会静默变回"请按开始测量"，
 * 用户只看到进度条消失，误以为"测完了但没数据"。停 3 轮(15s)把失败
 * 明确摆出来 */
#define PPG_SESSION_FAILED_ROUNDS  3

/* 按钮按下后等手指的超时轮数：约 6 轮(30s)还没放手指就放弃回 IDLE，
 * 避免用户按了就走、状态永远挂在 WAIT_FINGER */
#define PPG_SESSION_WAIT_FINGER_ROUNDS  6

typedef struct
{
    ppg_session_state_t state;

    rt_uint32_t wait_rounds;                /* WAIT_FINGER 已等待轮数 */
    rt_uint32_t settle_rounds;              /* 已丢弃的稳定期轮数 */
    rt_uint32_t bad_frames;                 /* 测量期连续坏帧数 */
    rt_uint32_t failed_rounds;              /* FAILED 态已停留轮数 */
    ppg_gate_result_t last_reject;          /* 最后一次被拒的原因，供 UI 提示 */

    /* 测量期样本缓冲，DONE 时取中位数 */
    int32_t     hr_buf[PPG_SESSION_MEASURE_FRAMES];
    int32_t     spo2_buf[PPG_SESSION_MEASURE_FRAMES];
    float       bv_buf[PPG_SESSION_MEASURE_FRAMES];
    rt_uint32_t n_frames;

    /* DONE 时的聚合结果 */
    int32_t     hr_result;
    int32_t     spo2_result;
    float       bv_result;
    rt_tick_t   done_tick;
    rt_tick_t   started_tick;
} ppg_session_t;

/* 单轮推进的返回值，告诉调用方本轮该做什么 */
typedef enum
{
    PPG_SESSION_ACT_NONE = 0,      /* 无事发生（空闲/丢弃中/累积中） */
    PPG_SESSION_ACT_STARTED,       /* 刚检测到手指，开始本次测量 */
    PPG_SESSION_ACT_PROGRESS,      /* 收到一个有效帧，但还没测完 */
    PPG_SESSION_ACT_COMPLETED,     /* 本轮测完，结果已写入 *_result，应发布 */
    PPG_SESSION_ACT_ABORTED,       /* 手指移开或坏帧过多，测量中断 */
} ppg_session_action_t;

/* ======================== API ======================== */

/** @brief 复位会话到 IDLE */
void ppg_session_reset(ppg_session_t *s);
void ppg_session_start(ppg_session_t *s, rt_tick_t now);
void ppg_session_cancel(ppg_session_t *s);
int ppg_session_check_timeout(ppg_session_t *s, rt_tick_t now);

/**
 * @brief 用本轮采集结果推进状态机
 *
 * @param s           会话
 * @param sqi         本轮 SQI（其中 dc 用于手指在位判定）
 * @param gate        本轮门控结论
 * @param hr          本轮心率（仅 gate==PASS 时有意义）
 * @param spo2        本轮血氧
 * @param bv          本轮血流速度
 * @param start_req   本轮是否收到"开始测量"按钮命令（M55 → M33）。
 *                    IDLE 态下只有同时 (start_req && 手指在位) 才启动测量；
 *                    没按钮就一直待命，不再"手指碰到就自动测"。
 * @return 本轮动作，调用方据此决定是否发布结果
 */
ppg_session_action_t ppg_session_feed(ppg_session_t *s,
                                      const ppg_sqi_t *sqi,
                                      ppg_gate_result_t gate,
                                      int32_t hr, int32_t spo2, float bv,
                                      rt_bool_t start_req);

/** @brief 状态转可读字符串（日志/UI 用） */
const char *ppg_session_state_str(ppg_session_state_t st);

/** @brief 测量进度 0-100（UI 进度条用；IDLE/DONE 时分别为 0/100） */
int ppg_session_progress(const ppg_session_t *s);

#endif /* __PPG_SESSION_H__ */
