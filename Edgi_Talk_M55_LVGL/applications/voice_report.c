/*
 * voice_report.c — 测量结果语音播报实现（说明见 voice_report.h）
 */

#include <rtthread.h>
#include <rtdevice.h>

#ifdef RT_USING_AUDIO

#include "voice_report.h"
#include "voice_data.h"
#include "ui/ui_data.h"
#include <string.h>

#define DBG_TAG "voice"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define VOICE_DEVICE      "sound0"
#define VOICE_RATE        16000   /* 与 voice_data.c 的 PCM 一致，勿改 */
#define VOICE_CHUNK       1600    /* 每次写入采样数(100ms)，同 audio_test.c */
#define VOICE_MAX_SEGS    24      /* 一次播报最多几段（"测量完成"+三项指标够用） */

/*
 * 播放用中转 buffer。
 *
 * 为什么要拷一次而不把 flash 地址直接丢给 rt_device_write：
 * PCM 在 .rodata → 外部 QSPI flash(0x60xxxxxx)。I2S 走 DMA，从 SMIF XIP 区
 * 直接 DMA 取数在本平台未经验证，且 QSPI 时序抖动可能造成欠载爆音。
 * 先拷进 SRAM 再喂 DMA 是稳妥做法，代价仅 3.2KB + 一次 memcpy。
 * audio_test.c 的 beep 同样用 RAM buffer。
 */
static int16_t s_chunk[VOICE_CHUNK];

/* 数字 0~9 → 片段 id。顺序与 voice_data.h 的枚举一致 */
static const voice_seg_id_t s_digit[10] = {
    VOICE_ZERO, VOICE_ONE, VOICE_TWO, VOICE_THREE, VOICE_FOUR,
    VOICE_FIVE, VOICE_SIX, VOICE_SEVEN, VOICE_EIGHT, VOICE_NINE,
};

/**
 * 整数 → 片段序列，中文读法。
 *
 * 中文数字不是逐位念的，规则要照顾：
 *   5   → 五          （不是"零五"）
 *   10  → 十          （不是"一十"）
 *   15  → 十五        （不是"一十五"）
 *   75  → 七十五
 *   80  → 八十        （不带尾零）
 *   100 → 一百
 *   105 → 一百零五    （中间的零要念）
 *   110 → 一百一十    （★ 百位之后的"一十"要念"一"，与独立的"十五"相反）
 *   120 → 一百二十
 * 只覆盖 0~999：心率/血氧/体温整数部分都在这个范围内。
 *
 * ★ 110 那条是 PC 复刻验证抓出来的（scratchpad 里跑 0~999 全量对照）：
 * 初版按"t>1 才念十位数字"处理，把 110 念成"一百十"。上板听大概率会被
 * 当成 TTS 口音放过去 —— 这类语言规则 bug 必须在 PC 上用表驱动核对。
 */
static int num_to_segs(int n, voice_seg_id_t *out, int cap)
{
    int k = 0;
    int has_hundred = 0;

    if (n < 0 || n > 999 || cap < 6)
        return -1;

    if (n == 0)
    {
        out[k++] = VOICE_ZERO;
        return k;
    }

    if (n >= 100)
    {
        out[k++] = s_digit[n / 100];
        out[k++] = VOICE_HUNDRED;
        has_hundred = 1;
        n %= 100;
        if (n == 0)
            return k;                 /* 200 → 二百 */
        if (n < 10)
            out[k++] = VOICE_ZERO;    /* 105 → 一百零五：中间的零必须念 */
    }

    if (n >= 10)
    {
        int t = n / 10;
        /* t==1 时念不念"一"取决于前面有没有百位：
         *   15  → 十五       (独立的十几，不念"一")
         *   110 → 一百一十   (百位之后，必须念"一") */
        if (t > 1 || has_hundred)
            out[k++] = s_digit[t];
        out[k++] = VOICE_TEN;
        n %= 10;
    }

    if (n > 0)
        out[k++] = s_digit[n];

    return k;
}

/* 把一串片段连续写进 sound0。设备已 open。 */
static void play_segs(rt_device_t dev, const voice_seg_id_t *ids, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        const voice_seg_t *s;
        uint32_t off;

        if (ids[i] < 0 || ids[i] >= VOICE_SEG_COUNT)
            continue;
        s = &g_voice_segs[ids[i]];

        for (off = 0; off < s->samples; off += VOICE_CHUNK)
        {
            uint32_t n_smp = s->samples - off;
            if (n_smp > VOICE_CHUNK)
                n_smp = VOICE_CHUNK;

            rt_memcpy(s_chunk, s->pcm + off, n_smp * sizeof(int16_t));
            rt_device_write(dev, 0, s_chunk, n_smp * sizeof(int16_t));
        }
    }
}

/*
 * 播放互斥。
 *
 * 2026-07-17 对抗审(rtos lens)指出：voice_say_number/voice_report_done/
 * voice_report_result 都是**公开导出 API**，而它们共用 static 的 s_chunk 并各自
 * open/write/close 同一个 sound0 —— 当前只有播报线程一个调用者所以没触发，
 * 但将来任何人加个诊断命令或第二个按钮直接调，就会和正在播的线程踩同一 buffer
 * (PCM 交错损坏) 并重复 open 设备。
 *
 * 不能只在注释里警告调用者"别乱调" —— 那种约定迟早被违反。在函数体内加锁，
 * 让 API 自身是安全的。播报本就串行(同时只该有一个声音)，锁的代价为零。
 */
static rt_mutex_t s_play_lock;

static rt_mutex_t voice_play_lock(void)
{
    /* 惰性创建：voice_report_init 里建更好，但那样 init 失败时这几个 API 就裸奔了 */
    if (s_play_lock == RT_NULL)
        s_play_lock = rt_mutex_create("vplay", RT_IPC_FLAG_PRIO);
    return s_play_lock;
}

/* 打开并配置 sound0。返回 RT_NULL 表示失败。步骤同 audio_test.c：先 configure 再 open */
static rt_device_t voice_open(void)
{
    struct rt_audio_caps caps = {0};
    rt_device_t dev = rt_device_find(VOICE_DEVICE);

    if (dev == RT_NULL)
    {
        LOG_E("'%s' not found — audio 未启用? 见 rtconfig.h 的 Audio 宏", VOICE_DEVICE);
        return RT_NULL;
    }

    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = VOICE_RATE;
    caps.udata.config.channels   = 1;
    caps.udata.config.samplebits = 16;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps);

    if (rt_device_open(dev, RT_DEVICE_OFLAG_WRONLY) != RT_EOK)
    {
        LOG_E("open '%s' failed", VOICE_DEVICE);
        return RT_NULL;
    }
    return dev;
}

int voice_say_number(int n)
{
    voice_seg_id_t ids[8];
    rt_device_t dev;
    rt_mutex_t  lk;
    int k = num_to_segs(n, ids, sizeof(ids) / sizeof(ids[0]));

    if (k <= 0)
        return -RT_EINVAL;

    lk = voice_play_lock();
    if (lk) rt_mutex_take(lk, RT_WAITING_FOREVER);

    dev = voice_open();
    if (dev == RT_NULL)
    {
        if (lk) rt_mutex_release(lk);
        return -RT_ERROR;
    }

    play_segs(dev, ids, k);
    rt_device_close(dev);
    if (lk) rt_mutex_release(lk);
    return RT_EOK;
}

void voice_report_done(void)
{
    voice_seg_id_t ids[1] = { VOICE_DONE };
    rt_device_t dev;
    rt_mutex_t  lk = voice_play_lock();

    if (lk) rt_mutex_take(lk, RT_WAITING_FOREVER);
    dev = voice_open();
    if (dev == RT_NULL)
    {
        if (lk) rt_mutex_release(lk);
        return;
    }
    play_segs(dev, ids, 1);
    rt_device_close(dev);
    if (lk) rt_mutex_release(lk);
}

void voice_report_result(int hr, int hr_valid,
                         int spo2, int spo2_valid,
                         float temp_c, int temp_valid)
{
    voice_seg_id_t ids[VOICE_MAX_SEGS];
    rt_device_t dev;
    int k = 0;
    int n_item = 0;

    ids[k++] = VOICE_DONE;

    /*
     * 各项独立判 valid。无效的整项跳过而不是播 0 ——
     * 屏幕显示 "--" 用户一眼看出没数，但语音说"心率零次每分"会被当成真结果。
     * 体检设备编数是硬伤，语音比屏幕更容易被当真（听不见 valid 标记）。
     */
    if (hr_valid && hr > 0 && hr <= 999)
    {
        int t;
        ids[k++] = VOICE_HR;
        t = num_to_segs(hr, &ids[k], VOICE_MAX_SEGS - k - 2);
        if (t > 0) { k += t; ids[k++] = VOICE_BPM; n_item++; }
        else       { k--; }              /* 拼不下就撤回"心率"，不留半句 */
    }

    if (spo2_valid && spo2 > 0 && spo2 <= 100)
    {
        int t;
        ids[k++] = VOICE_SPO2;
        ids[k++] = VOICE_PERCENT;        /* 中文是"百分之九十八"，单位在前 */
        t = num_to_segs(spo2, &ids[k], VOICE_MAX_SEGS - k - 1);
        if (t > 0) { k += t; n_item++; }
        else       { k -= 2; }
    }

    if (temp_valid && temp_c > 20.0f && temp_c < 45.0f)
    {
        int whole = (int)temp_c;
        int frac  = (int)((temp_c - (float)whole) * 10.0f + 0.5f);
        int t;

        if (frac >= 10) { whole += 1; frac = 0; }   /* 36.96 → 37.0，别念成"三十六点十" */

        ids[k++] = VOICE_TEMP;
        t = num_to_segs(whole, &ids[k], VOICE_MAX_SEGS - k - 3);
        if (t > 0)
        {
            k += t;
            ids[k++] = VOICE_POINT;
            ids[k++] = s_digit[frac];
            ids[k++] = VOICE_DEGREE;
            n_item++;
        }
        else { k--; }
    }

    LOG_I("report: %d segs (hr=%d/%d spo2=%d/%d temp=%d.%d/%d)",
          k, hr, hr_valid, spo2, spo2_valid,
          (int)temp_c, (int)((temp_c - (int)temp_c) * 10), temp_valid);

    {
        rt_mutex_t lk = voice_play_lock();
        if (lk) rt_mutex_take(lk, RT_WAITING_FOREVER);
        dev = voice_open();
        if (dev == RT_NULL)
        {
            if (lk) rt_mutex_release(lk);
            return;
        }
        play_segs(dev, ids, k);
        rt_device_close(dev);
        if (lk) rt_mutex_release(lk);
    }

    LOG_I("report done (%d items spoken)", n_item);
}

/* ------------------------------------------------------------------ */
/*  异步播报线程                                                        */
/* ------------------------------------------------------------------ */

/*
 * 为什么要单独一个线程：播报阻塞 3~5 秒，而唯一的触发点(ui_data.c 的 IPC 轮询)
 * 跑在 LVGL 线程里 —— 在那儿直接播会把整个界面卡死 5 秒，进度条、触摸全停。
 */
static rt_sem_t s_req_sem;
static volatile int s_busy;

void voice_report_request(void)
{
    if (s_req_sem == RT_NULL)
        return;
    if (s_busy)
        return;              /* 正在播就忽略，别排队堆积 */
    rt_sem_release(s_req_sem);
}

static void voice_thread_entry(void *param)
{
    (void)param;

    while (1)
    {
        struct ui_health_data snapshot;
        const struct ui_health_data *d = &snapshot;
        int i;

        rt_sem_take(s_req_sem, RT_WAITING_FOREVER);
        s_busy = 1;

        /* 从同一次快照取数值和有效标志；请求由 UI 发布新结果后触发。 */
        ui_data_copy(&snapshot);
        for (i = 0; i < 60 && (d == RT_NULL || !d->hr_valid); i++)
        {
            rt_thread_mdelay(100);
            ui_data_copy(&snapshot);
        }

        if (d != RT_NULL)
        {
            /* 字段口径以 ui_data.h 为准：spo2 是 float → 四舍五入成整数播报 */
            voice_report_result(d->heart_rate, d->hr_valid,
                                (int)(d->spo2 + 0.5f), d->spo2_valid,
                                d->body_temp, d->body_temp_valid);
        }
        else
        {
            voice_report_done();     /* 兜底：至少让用户知道测完了 */
        }

        s_busy = 0;
    }
}

static int voice_report_init(void)
{
    rt_thread_t tid;

    s_req_sem = rt_sem_create("voice", 0, RT_IPC_FLAG_FIFO);
    if (s_req_sem == RT_NULL)
    {
        LOG_E("sem create failed");
        return -RT_ERROR;
    }

    /* 栈 2048：play_segs 的 buffer 是 static(3.2KB 不进栈)，这里只有几十个
     * voice_seg_id_t + 局部量。优先级 25 同 audio_test 的自测线程 —— 播报
     * 是软实时，不该抢 LVGL/传感器的 CPU。
     *
     * LVGL 线程实际优先级 = 21（`lv_rt_thread_port.c:29` 的
     * PKG_LVGL_THREAD_PRIO = RT_THREAD_PRIORITY_MAX*2/3，而 .config 里
     * RT_THREAD_PRIORITY_MAX=32）。RT-Thread 数值越小优先级越高，25 > 21，
     * 故本线程不会抢占 LVGL。（此处原注释写"LVGL(20)"是我拍的近似值，
     * 2026-07-17 对抗审查出实为 21 —— 结论不变但数字曾是错的。） */
    tid = rt_thread_create("voice", voice_thread_entry, RT_NULL, 2048, 25, 10);
    if (tid == RT_NULL)
    {
        LOG_E("thread create failed");
        return -RT_ERROR;
    }
    rt_thread_startup(tid);
    LOG_I("voice report ready (%d segs in flash)", VOICE_SEG_COUNT);
    return RT_EOK;
}
INIT_APP_EXPORT(voice_report_init);

#endif /* RT_USING_AUDIO */
