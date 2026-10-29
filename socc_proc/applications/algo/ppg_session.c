/*
 * PPG 测量会话状态机 — 实现
 *
 * 设计依据见 ppg_session.h 顶部。
 */

#include <rtthread.h>
#include "ppg_session.h"

#define DBG_TAG "ppg_session"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 手指是否在位。复用 SQI 的 DC 门限，保持与 ppg_sqi_gate 判据一致 */
static rt_bool_t finger_present(const ppg_sqi_t *sqi)
{
    return (rt_bool_t)(sqi != RT_NULL && sqi->dc >= PPG_DC_FINGER_MIN);
}

/* 3 元素中位数。样本数固定且极小，直接比较比排序更清楚 */
static int32_t median3_i32(const int32_t *v)
{
    int32_t a = v[0], b = v[1], c = v[2];

    if (a > b) { int32_t t = a; a = b; b = t; }
    if (b > c) { int32_t t = b; b = c; c = t; }
    if (a > b) { b = a; }
    return b;
}

static float median3_f(const float *v)
{
    float a = v[0], b = v[1], c = v[2];

    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { b = a; }
    return b;
}

void ppg_session_reset(ppg_session_t *s)
{
    RT_ASSERT(s);
    rt_memset(s, 0, sizeof(*s));
    s->state = PPG_SESSION_IDLE;
}

void ppg_session_start(ppg_session_t *s, rt_tick_t now)
{
    ppg_session_reset(s);
    s->started_tick=now;
    s->state=PPG_SESSION_WAIT_FINGER;
}

void ppg_session_cancel(ppg_session_t *s)
{
    ppg_session_reset(s);
    s->state=PPG_SESSION_CANCELLED;
}

int ppg_session_check_timeout(ppg_session_t *s, rt_tick_t now)
{
    if ((s->state==PPG_SESSION_WAIT_FINGER || s->state==PPG_SESSION_SETTLING ||
         s->state==PPG_SESSION_MEASURING) &&
        now-s->started_tick >= rt_tick_from_millisecond(90000)) {
        ppg_session_reset(s);
        s->state=PPG_SESSION_FAILED;
        s->last_reject=PPG_GATE_REJ_TIMEOUT;
        return 1;
    }
    return 0;
}

const char *ppg_session_state_str(ppg_session_state_t st)
{
    switch (st)
    {
    case PPG_SESSION_IDLE:        return "idle";
    case PPG_SESSION_WAIT_FINGER: return "wait_finger";
    case PPG_SESSION_SETTLING:    return "settling";
    case PPG_SESSION_MEASURING:   return "measuring";
    case PPG_SESSION_DONE:        return "done";
    case PPG_SESSION_FAILED:      return "failed";
    case PPG_SESSION_CANCELLED:   return "cancelled";
    default:                      return "unknown";
    }
}

int ppg_session_progress(const ppg_session_t *s)
{
    if (s == RT_NULL)
        return 0;

    switch (s->state)
    {
    case PPG_SESSION_IDLE:
    case PPG_SESSION_WAIT_FINGER:
    case PPG_SESSION_FAILED:
        return 0;
    case PPG_SESSION_SETTLING:
        /* 稳定期占总进度的前 25% */
        return (int)(25 * s->settle_rounds / PPG_SESSION_SETTLE_ROUNDS);
    case PPG_SESSION_MEASURING:
        return 25 + (int)(75 * s->n_frames / PPG_SESSION_MEASURE_FRAMES);
    case PPG_SESSION_DONE:
        return 100;
    default:
        return 0;
    }
}

ppg_session_action_t ppg_session_feed(ppg_session_t *s,
                                      const ppg_sqi_t *sqi,
                                      ppg_gate_result_t gate,
                                      int32_t hr, int32_t spo2, float bv,
                                      rt_bool_t start_req)
{
    rt_bool_t present;

    RT_ASSERT(s);

    present = finger_present(sqi);

    /*
     * 手指移开的处理。仅在"已经贴合过并在推进"的两个态才算中断需要报 ABORTED；
     * WAIT_FINGER 态本来就在等手指，手指不在是常态，不能因此复位（否则用户按了
     * 按钮还没来得及放手指，状态就被冲掉了）。
     */
    if (!present)
    {
        if (s->state == PPG_SESSION_MEASURING || s->state == PPG_SESSION_SETTLING)
        {
            ppg_gate_result_t why = PPG_GATE_REJ_NO_FINGER;

            LOG_I("measurement aborted: finger removed (%u/%u frames collected)",
                  s->n_frames, (rt_uint32_t)PPG_SESSION_MEASURE_FRAMES);
            ppg_session_reset(s);
            s->state         = PPG_SESSION_FAILED;
            s->failed_rounds = 0;
            s->last_reject   = why;
            return PPG_SESSION_ACT_ABORTED;
        }
        /* IDLE / DONE / WAIT_FINGER / FAILED 态下手指不在：
         * DONE 手指移开 = 正常收尾回 IDLE；FAILED 继续停留（下面 switch 里计时）；
         * WAIT_FINGER 继续等（下面 switch 里处理超时）；IDLE 保持 */
        if (s->state == PPG_SESSION_DONE)
            ppg_session_reset(s);
    }

    switch (s->state)
    {
    case PPG_SESSION_IDLE:
        /* 待命。只有用户按下"开始测量"才启动，不再手指碰到就自动测。
         * 按下瞬间手指已在 → 直接进稳定期；还没放 → 先进 WAIT_FINGER 等 */
        if (start_req)
        {
            s->n_frames   = 0;
            s->bad_frames = 0;
            if (present)
            {
                s->state         = PPG_SESSION_SETTLING;
                s->settle_rounds = 0;
                LOG_I("measure started (DC=%u), settling %d round(s)...",
                      sqi->dc, PPG_SESSION_SETTLE_ROUNDS);
            }
            else
            {
                s->state       = PPG_SESSION_WAIT_FINGER;
                s->wait_rounds = 0;
                LOG_I("measure requested, waiting for finger...");
            }
            return PPG_SESSION_ACT_STARTED;
        }
        return PPG_SESSION_ACT_NONE;

    case PPG_SESSION_WAIT_FINGER:
        /* 已按开始，等手指贴上。放上了就进稳定期；超时没放就放弃回 IDLE。
         * 期间再按一次按钮视作重置等待计时 */
        if (start_req)
            s->wait_rounds = 0;
        if (present)
        {
            s->state         = PPG_SESSION_SETTLING;
            s->settle_rounds = 0;
            LOG_I("finger detected (DC=%u), settling...", sqi->dc);
            return PPG_SESSION_ACT_NONE;
        }
        if (++s->wait_rounds >= PPG_SESSION_WAIT_FINGER_ROUNDS)
        {
            LOG_W("measure cancelled: no finger within timeout");
            ppg_session_reset(s);
            s->state         = PPG_SESSION_FAILED;
            s->failed_rounds = 0;
            s->last_reject   = PPG_GATE_REJ_NO_FINGER;
            return PPG_SESSION_ACT_ABORTED;
        }
        return PPG_SESSION_ACT_NONE;

    case PPG_SESSION_SETTLING:
        s->settle_rounds++;
        if (s->settle_rounds >= PPG_SESSION_SETTLE_ROUNDS)
        {
            s->state      = PPG_SESSION_MEASURING;
            s->n_frames   = 0;
            s->bad_frames = 0;
            LOG_I("baseline settled, measuring (need %d valid frames)...",
                  PPG_SESSION_MEASURE_FRAMES);
        }
        return PPG_SESSION_ACT_NONE;

    case PPG_SESSION_MEASURING:
        if (gate != PPG_GATE_PASS)
        {
            /* 坏帧不计入，但连续太多说明姿势有问题，回退重来而非无限等 */
            s->bad_frames++;
            s->last_reject = gate;   /* 记下原因，FAILED 态要告诉用户为什么 */
            if (s->bad_frames >= PPG_SESSION_MAX_BAD_FRAMES)
            {
                LOG_W("measurement aborted: %u consecutive bad frames [%s], keep finger still",
                      s->bad_frames, ppg_gate_reason_str(gate));
                ppg_session_reset(s);
                s->state         = PPG_SESSION_FAILED;
                s->failed_rounds = 0;
                s->last_reject   = gate;
                return PPG_SESSION_ACT_ABORTED;
            }
            return PPG_SESSION_ACT_NONE;
        }

        s->bad_frames = 0;
        s->last_reject = PPG_GATE_PASS; /* 接触恢复后不再持续显示上一坏帧提示 */

        if (s->n_frames < PPG_SESSION_MEASURE_FRAMES)
        {
            s->hr_buf[s->n_frames]   = hr;
            s->spo2_buf[s->n_frames] = spo2;
            s->bv_buf[s->n_frames]   = bv;
            s->n_frames++;
        }

        if (s->n_frames >= PPG_SESSION_MEASURE_FRAMES)
        {
            /* 取中位数而非均值：PPG 偶发的重搏波误检会让个别帧心率翻倍，
             * 均值会被这类离群值拉偏，中位数直接把它压掉 */
            s->hr_result   = median3_i32(s->hr_buf);
            s->spo2_result = median3_i32(s->spo2_buf);
            s->bv_result   = median3_f(s->bv_buf);
            s->done_tick   = rt_tick_get();
            s->state       = PPG_SESSION_DONE;

            LOG_I("measurement complete: HR=%d SpO2=%d BV=%.2f (median of %d frames: HR %d/%d/%d)",
                  s->hr_result, s->spo2_result, s->bv_result, PPG_SESSION_MEASURE_FRAMES,
                  s->hr_buf[0], s->hr_buf[1], s->hr_buf[2]);
            return PPG_SESSION_ACT_COMPLETED;
        }

        return PPG_SESSION_ACT_PROGRESS;

    case PPG_SESSION_CANCELLED:
    case PPG_SESSION_FAILED:
        /* 失败态：停留数轮让 UI 把失败摆给用户看，其间随时可再按重测 */
        if (start_req)
        {
            s->n_frames   = 0;
            s->bad_frames = 0;
            if (present)
            {
                s->state         = PPG_SESSION_SETTLING;
                s->settle_rounds = 0;
                LOG_I("retry measure (DC=%u), settling...", sqi->dc);
            }
            else
            {
                s->state       = PPG_SESSION_WAIT_FINGER;
                s->wait_rounds = 0;
                LOG_I("retry measure, waiting for finger...");
            }
            return PPG_SESSION_ACT_STARTED;
        }
        if (s->state==PPG_SESSION_FAILED && ++s->failed_rounds >= PPG_SESSION_FAILED_ROUNDS)
            ppg_session_reset(s);
        return PPG_SESSION_ACT_NONE;

    case PPG_SESSION_DONE:
        /* 结果已出。再按一次"开始测量"→ 立即重测（手指还在就直接进稳定期），
         * 不用先移开手指。不按则保持结果不动，不自动重测（避免数字持续跳） */
        if (start_req)
        {
            s->n_frames      = 0;
            s->bad_frames    = 0;
            s->settle_rounds = 0;
            s->state         = PPG_SESSION_SETTLING;
            LOG_I("re-measure requested (DC=%u), settling...", sqi->dc);
            return PPG_SESSION_ACT_STARTED;
        }
        return PPG_SESSION_ACT_NONE;

    default:
        ppg_session_reset(s);
        return PPG_SESSION_ACT_NONE;
    }
}
