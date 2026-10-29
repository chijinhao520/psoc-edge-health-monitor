/*
 * audio_test.c — 音频播放通路点亮验证(sound0)
 *
 * ⚠️ 本工程 M55 禁用了 MSH/FINSH(uart2 输入留给 M33),没有交互式命令行,
 *    所以用「开机自动播一声」验证,而不是 MSH 命令:
 *    烧录后约 4 秒,自动从喇叭播一段 1kHz 正弦音,并在 uart2 打印日志。
 *    验证 ES8388 → 功放(SPEAKER_OE,board.c en_gpio 上电已使能)→ 喇叭 通路。
 *
 * 特点:零依赖 —— 不需要文件系统 / WAV 文件,PCM 现场生成。
 * 验证通过后:注释掉下面的 AUDIO_BOOT_SELFTEST 宏即可关闭开机自测。
 */
#include <rtthread.h>
#include <rtdevice.h>

#ifdef RT_USING_AUDIO
#include <math.h>

/* 验证阶段开关:开机自动播一声。验证 OK 后注释此行关闭 */
#define AUDIO_BOOT_SELFTEST
#define AUDIO_SELFTEST_DELAY_MS   4000    /* 延迟,等系统/audio 设备就绪 */

#define BEEP_DEVICE   "sound0"
#define BEEP_RATE     16000               /* 采样率,与 sound0 配置一致 */
#define BEEP_AMP      8000                /* 幅度(< 32767,留余量防削顶) */
#define BEEP_CHUNK    1600                /* 每次写入采样数(100ms @16kHz) */

/* 播放一段正弦 tone。freq: 频率(Hz),ms: 时长。返回 0 成功。可被其它模块调用 */
int audio_beep_play(int freq, int ms)
{
    if (freq < 100 || freq > 8000) freq = 1000;
    if (ms < 100  || ms > 10000)   ms   = 1500;

    rt_device_t dev = rt_device_find(BEEP_DEVICE);
    if (dev == RT_NULL)
    {
        rt_kprintf("[audio_beep] device '%s' not found -- enable Audio in RT-Thread Settings first\n", BEEP_DEVICE);
        return -RT_ERROR;
    }

    /* 配置 16kHz / 16bit / 单声道;顺序同官方 xz_sound_init:先 configure 再 open */
    struct rt_audio_caps caps = {0};
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = BEEP_RATE;
    caps.udata.config.channels   = 1;
    caps.udata.config.samplebits = 16;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &caps);

    if (rt_device_open(dev, RT_DEVICE_OFLAG_WRONLY) != RT_EOK)
    {
        rt_kprintf("[audio_beep] open '%s' failed\n", BEEP_DEVICE);
        return -RT_ERROR;
    }

    rt_int16_t *buf = rt_malloc(BEEP_CHUNK * sizeof(rt_int16_t));
    if (buf == RT_NULL)
    {
        rt_device_close(dev);
        rt_kprintf("[audio_beep] malloc failed\n");
        return -RT_ENOMEM;
    }

    /* 生成正弦波(1kHz 时 BEEP_CHUNK 恰为整周期,循环播放无相位跳变) */
    for (int i = 0; i < BEEP_CHUNK; i++)
    {
        float t = (float)i / BEEP_RATE;
        buf[i] = (rt_int16_t)(BEEP_AMP * sinf(2.0f * 3.14159265f * (float)freq * t));
    }

    int chunks = (BEEP_RATE * ms / 1000) / BEEP_CHUNK;
    rt_kprintf("[audio_beep] playing %dHz / %dms (%d chunks)...\n", freq, ms, chunks);
    for (int c = 0; c < chunks; c++)
        rt_device_write(dev, 0, buf, BEEP_CHUNK * sizeof(rt_int16_t));

    rt_free(buf);
    rt_device_close(dev);
    rt_kprintf("[audio_beep] done\n");
    return RT_EOK;
}

#ifdef AUDIO_BOOT_SELFTEST
/* 开机自测线程:延迟等就绪后播一声(M55 无 MSH,用自启动代替手敲命令) */
static void audio_selftest_entry(void *param)
{
    (void)param;
    rt_thread_mdelay(AUDIO_SELFTEST_DELAY_MS);
    rt_kprintf("[audio_beep] selftest: preparing to play...\n");
    audio_beep_play(1000, 1500);
}

static int audio_selftest_init(void)
{
    rt_thread_t tid = rt_thread_create("beep_test", audio_selftest_entry,
                                       RT_NULL, 2048, 25, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
    return 0;
}
INIT_APP_EXPORT(audio_selftest_init);
#endif /* AUDIO_BOOT_SELFTEST */

/* 若将来开启 MSH,可用 `audio_beep [freq] [ms]` 手动播放 */
#ifdef RT_USING_FINSH
#include <finsh.h>
#include <stdlib.h>
static int audio_beep(int argc, char **argv)
{
    int freq = (argc >= 2) ? atoi(argv[1]) : 1000;
    int ms   = (argc >= 3) ? atoi(argv[2]) : 1500;
    return audio_beep_play(freq, ms);
}
MSH_CMD_EXPORT(audio_beep, play test tone on sound0: audio_beep [freq_hz] [ms]);
#endif

#endif /* RT_USING_AUDIO */
