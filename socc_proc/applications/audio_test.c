/*
 * audio_test.c - sound0 发声验证 (M33 / socc_proc)
 *
 * 照 Edgi_Talk_M33_Audio 模板: 先设音量(MIXER_VOLUME)+格式(DSP_PARAM), 再 open/write。
 * M33 有 MSH, 用命令触发:  audio_beep [freq_hz] [ms]
 * 用方波(不依赖 libm/sinf), 只为验证"喇叭能不能出声"。
 */
#include <rtthread.h>
#include <rtdevice.h>

#ifdef RT_USING_AUDIO
#include <stdlib.h>

#define BEEP_DEVICE   "sound0"
#define BEEP_RATE     16000
#define BEEP_AMP      8000
#define BEEP_CHUNK    1600

static int audio_beep(int argc, char **argv)
{
    int freq = (argc >= 2) ? atoi(argv[1]) : 1000;
    int ms   = (argc >= 3) ? atoi(argv[2]) : 1500;
    if (freq < 100 || freq > 8000) freq = 1000;
    if (ms < 100  || ms > 10000)   ms   = 1500;

    rt_device_t dev = rt_device_find(BEEP_DEVICE);
    if (dev == RT_NULL)
    {
        rt_kprintf("[beep] sound0 not found\n");
        return -RT_ERROR;
    }

    /* 1) volume (same as official M33_Audio) */
    struct rt_audio_caps vol = {0};
    vol.main_type   = AUDIO_TYPE_MIXER;
    vol.sub_type    = AUDIO_MIXER_VOLUME;
    vol.udata.value = 70;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &vol);

    /* 2) format: 16kHz / 16bit / mono */
    struct rt_audio_caps fmt = {0};
    fmt.main_type = AUDIO_TYPE_OUTPUT;
    fmt.sub_type  = AUDIO_DSP_PARAM;
    fmt.udata.config.samplerate = BEEP_RATE;
    fmt.udata.config.channels   = 1;
    fmt.udata.config.samplebits = 16;
    rt_device_control(dev, AUDIO_CTL_CONFIGURE, &fmt);

    if (rt_device_open(dev, RT_DEVICE_OFLAG_WRONLY) != RT_EOK)
    {
        rt_kprintf("[beep] open failed\n");
        return -RT_ERROR;
    }

    rt_int16_t *buf = rt_malloc(BEEP_CHUNK * sizeof(rt_int16_t));
    if (buf == RT_NULL)
    {
        rt_device_close(dev);
        rt_kprintf("[beep] malloc failed\n");
        return -RT_ENOMEM;
    }

    /* square wave (no libm) */
    int period = BEEP_RATE / freq;
    if (period < 2) period = 2;
    for (int i = 0; i < BEEP_CHUNK; i++)
        buf[i] = ((i % period) < (period / 2)) ? (rt_int16_t)BEEP_AMP : (rt_int16_t)(-BEEP_AMP);

    int chunks = (BEEP_RATE * ms / 1000) / BEEP_CHUNK;
    rt_kprintf("[beep] playing %dHz %dms (%d chunks) ...\n", freq, ms, chunks);
    for (int c = 0; c < chunks; c++)
        rt_device_write(dev, 0, buf, BEEP_CHUNK * sizeof(rt_int16_t));

    rt_free(buf);
    rt_device_close(dev);
    rt_kprintf("[beep] done\n");
    return RT_EOK;
}
MSH_CMD_EXPORT(audio_beep, play test tone on sound0: audio_beep [freq_hz] [ms]);

#endif /* RT_USING_AUDIO */
