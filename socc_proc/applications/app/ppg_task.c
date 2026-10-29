/*
 * PPG 采样与处理任务
 *
 * 负责：
 * 1. 初始化 MAX30102 传感器
 * 2. 以 100Hz 采集 500 个点（约 5 秒）
 * 3. 连续样本通过质量门控后计算心率、血氧
 * 4. 通过串口输出结果，便于调试验证
 */

#include <rtthread.h>
#include "../sensor/MAX30102.h"
#include "../sensor/tca9548a.h"
#include "../algo/ppg_process.h"
#include "../algo/ppg_sqi.h"
#include "../algo/ppg_session.h"
#include "../algo/ppg_downsample.h"
#include "sensor_data.h"
#include "ipc_shared_data.h"

#define DBG_TAG "ppg_task"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/*
 * 两颗 MAX30102 默认 I2C 地址都是 0x57，靠 TCA9548A mux 通道隔离区分：
 * CH0/CH1 任一路可用即运行；两路均可用时优先选通过质量门控的源。
 */
#define PPG_MUX_CH_PRIMARY   0
#define PPG_MUX_CH_SECONDARY 1

/*
 * 放到全局区，避免线程栈溢出。
 * 每个缓冲区 500 * 4 Byte = 2 KB，两个数组共约 4 KB。
 */
static rt_uint32_t g_ir_buf[BUFFER_SIZE];
static rt_uint32_t g_red_buf[BUFFER_SIZE];

/* CH1 辅 PPG 缓冲区，仅在第二颗 MAX30102 实际存在时使用 */
static rt_uint32_t g_ir_buf_ch1[BUFFER_SIZE];
static rt_uint32_t g_red_buf_ch1[BUFFER_SIZE];

/* T6: PPG 原始波形串口导出开关（默认关闭，MSH 命令 ppg_dump 切换） */
static rt_bool_t g_ppg_dump_en = RT_FALSE;

/* A2: 下采样结果缓冲（128×2 float, [0,1] 归一化），供 A6 IPC 传输到 M55 NPU */
static float g_ppg_ds[PPG_DS_LEN][PPG_DS_CHANNELS];

/* 波形无效时同时撤回 magic，消费者必须清掉上次结果。 */
static void invalidate_ppg(void)
{
    volatile ppg_waveform_ipc_t *wf = ipc_get_waveform_ptr();
    wf->magic = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    sensor_data_update_ppg(0, 0, 0.0f, RT_FALSE);
}

/* 即使没有传感器也受理请求，避免界面永久停在“测量中”。 */
static int handle_command(ppg_session_t *s, uint32_t *last_seq)
{
    volatile ppg_cmd_ipc_t *cmd=ipc_get_cmd_ptr();
    uint32_t seq=cmd->seq;
    int action=cmd->cmd;
    __asm__ volatile("dsb sy" ::: "memory");
    if (cmd->magic!=IPC_CMD_MAGIC || cmd->seq!=seq || seq==*last_seq) return PPG_CMD_NONE;
    *last_seq=seq;
    if (action!=PPG_CMD_START_MEASURE && action!=PPG_CMD_CANCEL_MEASURE) return PPG_CMD_NONE;
    invalidate_ppg();
    if (action==PPG_CMD_START_MEASURE) ppg_session_start(s,rt_tick_get());
    else ppg_session_cancel(s);
    sensor_data_begin_measurement(seq);
    sensor_data_update_session(s->state,0,0);
    return action;
}

static void fail_session(ppg_session_t *s, ppg_gate_result_t reason)
{
    if (s->state==PPG_SESSION_WAIT_FINGER || s->state==PPG_SESSION_SETTLING ||
        s->state==PPG_SESSION_MEASURING) {
        ppg_session_reset(s);
        s->state=PPG_SESSION_FAILED;
        s->last_reject=reason;
        invalidate_ppg();
        sensor_data_update_session(s->state,0,reason);
    }
}

static void flush_channels(tca9548a_device_t mux, max30102_device_t dev[2],
                           rt_bool_t good[2], rt_tick_t last_poll[2])
{
    for (int ch=0;ch<2;ch++) if (dev[ch]) {
        good[ch]=RT_FALSE;
        if (tca9548a_lock(mux)==RT_EOK) {
            good[ch]=tca9548a_select_channel(mux,ch)==RT_EOK && max30102_flush_fifo(dev[ch])==RT_EOK;
            tca9548a_unlock(mux);
        }
        last_poll[ch]=rt_tick_get();
    }
}

static rt_err_t collect_channel(tca9548a_device_t mux, max30102_device_t dev,
                                int channel, uint32_t *red, uint32_t *ir,
                                rt_uint16_t *used, rt_tick_t *last_poll)
{
    rt_uint16_t n = 0;
    rt_tick_t now;
    rt_err_t err;
    if (*used == BUFFER_SIZE) return RT_EOK;
    err = tca9548a_lock(mux);
    if (err != RT_EOK) return err;
    err = tca9548a_select_channel(mux, channel);
    /* 等锁和选通也消耗FIFO余量，时间检查必须在获得总线之后。 */
    now = rt_tick_get();
    if (err == RT_EOK && now - *last_poll > rt_tick_from_millisecond(200))
        err = -RT_EFULL;
    if (err == RT_EOK)
        err = max30102_read_batch(dev, red + *used, ir + *used, BUFFER_SIZE - *used, &n);
    *last_poll = now;
    if (err != RT_EOK)
    {
        /* 选通失败时不访问下游：可能仍连着另一颗同地址传感器。 */
        if (tca9548a_select_channel(mux, channel) == RT_EOK)
            max30102_flush_fifo(dev);
        *used = 0;
    }
    else *used += n;
    tca9548a_unlock(mux);
    return err;
}

static void ppg_thread_entry(void *param)
{
    tca9548a_device_t mux;
    max30102_device_t dev[2] = {RT_NULL, RT_NULL};
    uint32_t *red[2] = {g_red_buf, g_red_buf_ch1};
    uint32_t *ir[2] = {g_ir_buf, g_ir_buf_ch1};
    rt_tick_t last_poll[2]={0,0}, retry_tick[2]={0,0};
    rt_bool_t tried[2]={RT_FALSE,RT_FALSE}, recover[2]={RT_FALSE,RT_FALSE};
    uint32_t last_cmd_seq = 0;
    int selected = -1;
    ppg_session_t session;
    RT_UNUSED(param);
    rt_thread_mdelay(5000);
    ppg_session_reset(&session);
    mux=RT_NULL;
    while (1)
    {
        int command=handle_command(&session,&last_cmd_seq);
        if (command==PPG_CMD_START_MEASURE) selected=-1;
        if (!mux) mux=tca9548a_shared();
        if (!mux) {
            fail_session(&session,PPG_GATE_REJ_SENSOR);
            rt_thread_mdelay(200);
            continue;
        }
        /* 每窗核对关键配置，掉电后不能拿复位默认采样率继续计算。缺失源按2秒退避，
         * 单次探测不做500ms长重试；恢复成功重新配置并清FIFO。 */
        for (int ch=0;ch<2;ch++) {
            if (dev[ch] && tca9548a_lock(mux)==RT_EOK) {
                if (tca9548a_select_channel(mux,ch)!=RT_EOK || max30102_check_config(dev[ch])!=RT_EOK)
                    recover[ch]=RT_TRUE;
                tca9548a_unlock(mux);
            }
            if (recover[ch] && dev[ch]) {
                if (selected==ch) fail_session(&session,PPG_GATE_REJ_SENSOR);
                max30102_deinit(dev[ch]); dev[ch]=RT_NULL; recover[ch]=RT_FALSE;
            }
            if (!dev[ch] && (!tried[ch] || rt_tick_get()-retry_tick[ch]>=rt_tick_from_millisecond(2000))) {
                tried[ch]=RT_TRUE; retry_tick[ch]=rt_tick_get();
                if (tca9548a_lock(mux)==RT_EOK) {
                    if (tca9548a_select_channel(mux,ch)==RT_EOK) dev[ch]=max30102_try_init("i2c1");
                    tca9548a_unlock(mux);
                }
                last_poll[ch]=rt_tick_get();
            }
        }
        if (!dev[0] && !dev[1]) {
            fail_session(&session,PPG_GATE_REJ_SENSOR);
            rt_thread_mdelay(50);
            continue;
        }
        rt_uint16_t used[2] = {0, 0};
        rt_bool_t good[2] = {dev[0] != RT_NULL, dev[1] != RT_NULL};
        rt_tick_t begin = rt_tick_get();
        rt_bool_t start_req = command==PPG_CMD_START_MEASURE;
        rt_bool_t discarded = RT_FALSE;
        if (start_req) flush_channels(mux,dev,good,last_poll);
        ppg_sqi_t sqi[2] = {{0}, {0}};
        int32_t hr[2] = {-999,-999}, spo2[2] = {-999,-999};
        int8_t hv[2] = {0,0}, sv[2] = {0,0};
        ppg_gate_result_t gate[2] = {PPG_GATE_REJ_ALGO, PPG_GATE_REJ_ALGO};
        /* FIFO 是传感器时钟；每次最多读 8 点，释放 mux 后让其他任务运行。 */
        while ((good[0] && used[0] < BUFFER_SIZE) || (good[1] && used[1] < BUFFER_SIZE))
        {
            command=handle_command(&session,&last_cmd_seq);
            if (command==PPG_CMD_CANCEL_MEASURE) { discarded=RT_TRUE; break; }
            if (command==PPG_CMD_START_MEASURE) {
                    selected=-1;
                    start_req = RT_TRUE;
                    /* 新请求丢弃已经积累的前一会话窗口。 */
                    used[0] = used[1] = 0;
                    begin = rt_tick_get();
                    flush_channels(mux,dev,good,last_poll);
            }
            if (ppg_session_check_timeout(&session,rt_tick_get())) {
                invalidate_ppg();
                sensor_data_update_session(session.state,0,session.last_reject);
                discarded=RT_TRUE; break;
            }
            for (int ch = 0; ch < 2; ch++) if (good[ch]) {
                rt_err_t err=collect_channel(mux, dev[ch], ch, red[ch], ir[ch],&used[ch], &last_poll[ch]);
                if (err!=RT_EOK) { good[ch]=RT_FALSE; if (err!=-RT_EFULL) recover[ch]=RT_TRUE; }
            }
            if (rt_tick_get() - begin > rt_tick_from_millisecond(8000))
            {
                for (int ch=0;ch<2;ch++) if (good[ch] && used[ch]!=BUFFER_SIZE) {
                    good[ch]=RT_FALSE; recover[ch]=RT_TRUE;
                }
                break;
            }
            rt_thread_mdelay(20);
        }
        if (discarded) continue;
        for (int ch = 0; ch < 2; ch++) if (good[ch] && used[ch] == BUFFER_SIZE)
        {
            if (ppg_sqi_compute(ir[ch], BUFFER_SIZE, FS, &sqi[ch]) != RT_EOK) {
                good[ch] = RT_FALSE;
                continue;
            }
            /* 饱和/零码不能通过高 DC 选择器混入有效波形。 */
            for (int j = 0; j < BUFFER_SIZE; j++)
                if (ir[ch][j] == 0 || ir[ch][j] >= 0x3FFFFu ||
                    red[ch][j] == 0 || red[ch][j] >= 0x3FFFFu) good[ch] = RT_FALSE;
            if (!good[ch]) continue;
            maxim_heart_rate_and_oxygen_saturation(ir[ch], BUFFER_SIZE, red[ch],
                                                   &spo2[ch], &sv[ch], &hr[ch], &hv[ch], FS);
            gate[ch] = ppg_sqi_gate(&sqi[ch], hr[ch], spo2[ch], hv[ch], sv[ch]);
        }
        /* 算法计算期间收到的新开始请求由下一轮受理，当前旧窗口不得发布。 */
        volatile ppg_cmd_ipc_t *latest_cmd=ipc_get_cmd_ptr();
        __asm__ volatile("dsb sy" ::: "memory");
        if (latest_cmd->magic==IPC_CMD_MAGIC && latest_cmd->seq!=last_cmd_seq &&
            (latest_cmd->cmd==PPG_CMD_START_MEASURE || latest_cmd->cmd==PPG_CMD_CANCEL_MEASURE)) continue;
        if (ppg_session_check_timeout(&session,rt_tick_get())) {
            invalidate_ppg();
            sensor_data_update_session(session.state,0,session.last_reject);
            continue;
        }
        int best = -1;
        for (int ch = 0; ch < 2; ch++) if (good[ch] && used[ch] == BUFFER_SIZE)
        {
            if (best < 0 || (gate[ch] == PPG_GATE_PASS && gate[best] != PPG_GATE_PASS) ||
                ((gate[ch] == PPG_GATE_PASS) == (gate[best] == PPG_GATE_PASS) &&
                 sqi[ch].dc >= PPG_DC_FINGER_MIN &&
                 (sqi[best].dc < PPG_DC_FINGER_MIN || sqi[ch].score > sqi[best].score))) best = ch;
        }
        if (best < 0)
        {
            fail_session(&session,PPG_GATE_REJ_SENSOR);
            LOG_W("PPG discontinuity: discard window, retry required");
            rt_thread_mdelay(200);
            continue;
        }
        if (selected >= 0 && selected != best &&
            (session.state == PPG_SESSION_MEASURING || session.state == PPG_SESSION_SETTLING))
        {
            session.state = PPG_SESSION_SETTLING;
            session.n_frames = session.settle_rounds = session.bad_frames = 0;
        }
        selected = best;
        ppg_session_action_t action = ppg_session_feed(&session, &sqi[best], gate[best],
                                                       hr[best], spo2[best], 0.0f, start_req);
        if (action == PPG_SESSION_ACT_COMPLETED)
        {
            sensor_data_update_ppg(session.hr_result, session.spo2_result, 0.0f, RT_TRUE);
            ppg_downsample(red[best], ir[best], BUFFER_SIZE, g_ppg_ds);
            volatile ppg_waveform_ipc_t *wf = ipc_get_waveform_ptr();
            wf->sequence++;
            __asm__ volatile("dsb sy" ::: "memory");
            rt_memcpy((void *)wf->data, g_ppg_ds, sizeof(g_ppg_ds));
            wf->tick = rt_tick_get();
            wf->measurement_id = ipc_get_data_ptr()->ppg_measurement_id;
            wf->magic = IPC_PPG_MAGIC;
            __asm__ volatile("dsb sy" ::: "memory");
            wf->sequence++;
        }
        else if (action == PPG_SESSION_ACT_ABORTED || action == PPG_SESSION_ACT_STARTED)
            invalidate_ppg();
        sensor_data_update_session(session.state, ppg_session_progress(&session), session.last_reject);
        LOG_I("PPG CH%d [%s %d%%] HR=%d SpO2=%d SQI=%d gate=%s fs=%dHz",
              best, ppg_session_state_str(session.state), ppg_session_progress(&session),
              hr[best], spo2[best], sqi[best].score, ppg_gate_reason_str(gate[best]), FS);
        /* 调试逐行带索引输出，保持调度运行；长导出后的 FIFO 间断会被下一轮拒绝。 */
        if (g_ppg_dump_en)
        {
            for (int j = 0; j < BUFFER_SIZE; j++)
                rt_kprintf("[PPG_SAMPLE],CH%d,%d,%u,%u,%d\n", best, j, red[best][j], ir[best][j], FS);
        }
    }
}

int ppg_task_init(void)
{
    rt_thread_t tid = rt_thread_create(
        "ppg",
        ppg_thread_entry,
        RT_NULL,
        2048,
        12,
        10);

    if (tid == RT_NULL)
    {
        LOG_E("PPG thread create failed");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(ppg_task_init);

/* T6: MSH 命令切换 PPG 波形导出 */
static void ppg_dump(void)
{
    g_ppg_dump_en = !g_ppg_dump_en;
    rt_kprintf("PPG dump %s\n", g_ppg_dump_en ? "ON" : "OFF");
}
MSH_CMD_EXPORT(ppg_dump, Toggle PPG raw waveform serial dump);
