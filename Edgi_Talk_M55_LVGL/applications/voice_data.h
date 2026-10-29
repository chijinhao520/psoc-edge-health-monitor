/*
 * voice_data.h — 语音播报 PCM 片段索引（自动生成，勿手改）
 * 生成器: scratchpad/gen_voice.py
 */
#ifndef __VOICE_DATA_H__
#define __VOICE_DATA_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const int16_t *pcm;      /* 16kHz/16bit/mono，.rodata → 外部 flash */
    uint32_t       samples;  /* 采样点数（字节数 = samples*2） */
} voice_seg_t;

typedef enum
{
    VOICE_ZERO,      /* 零 */
    VOICE_ONE,       /* 一 */
    VOICE_TWO,       /* 二 */
    VOICE_THREE,     /* 三 */
    VOICE_FOUR,      /* 四 */
    VOICE_FIVE,      /* 五 */
    VOICE_SIX,       /* 六 */
    VOICE_SEVEN,     /* 七 */
    VOICE_EIGHT,     /* 八 */
    VOICE_NINE,      /* 九 */
    VOICE_TEN,       /* 十 */
    VOICE_HUNDRED,   /* 百 */
    VOICE_POINT,     /* 点 */
    VOICE_HR,        /* 心率 */
    VOICE_SPO2,      /* 血氧 */
    VOICE_TEMP,      /* 体温 */
    VOICE_BPM,       /* 次每分 */
    VOICE_PERCENT,   /* 百分之 */
    VOICE_DEGREE,    /* 度 */
    VOICE_DONE,      /* 测量完成 */
    VOICE_SEG_COUNT
} voice_seg_id_t;

extern const voice_seg_t g_voice_segs[VOICE_SEG_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* __VOICE_DATA_H__ */
