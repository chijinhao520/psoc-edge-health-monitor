/*
 * ui_cn_font.c — 中文 Tiny TTF 字体运行时加载
 *
 * 见 ui_cn_font.h。字体二进制在 fonts/noto_gb2312_ttf.c(const 数组，外部 flash)。
 */
#include "ui_cn_font.h"
#include <rtthread.h>

#define DBG_TAG "cnfont"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* GB2312 全集子集 TTF，定义在 fonts/noto_gb2312_ttf.c(.rodata -> 外部 flash) */
extern const uint8_t  noto_gb2312_ttf[];
extern const uint32_t noto_gb2312_ttf_size;

/* 最多缓存几个不同字号(报告卡片正文一个字号足够，留余量给标题等) */
#define UI_CN_FONT_MAX_SIZES    4
/* 每字号字形 LRU 缓存条数(一屏可见字数量级即可，缓存进 heap) */
#define UI_CN_FONT_GLYPH_CACHE  96

static struct {
    int        px;
    lv_font_t *font;
} s_cache[UI_CN_FONT_MAX_SIZES];
static int s_count = 0;

lv_font_t *ui_cn_font_get(int px)
{
    if (px <= 0)
        return NULL;

    /* 命中缓存 */
    for (int i = 0; i < s_count; i++)
        if (s_cache[i].px == px)
            return s_cache[i].font;

    /* 字号槽满：复用第一个，避免无限创建耗 heap */
    if (s_count >= UI_CN_FONT_MAX_SIZES) {
        LOG_W("size slots full (%d), reuse px=%d for req px=%d",
              UI_CN_FONT_MAX_SIZES, s_cache[0].px, px);
        return s_cache[0].font;
    }

    lv_font_t *font = lv_tiny_ttf_create_data_ex(
        (const void *)noto_gb2312_ttf,
        (size_t)noto_gb2312_ttf_size,
        px, LV_FONT_KERNING_NONE,
        UI_CN_FONT_GLYPH_CACHE);

    if (font == NULL) {
        LOG_E("lv_tiny_ttf_create_data_ex(px=%d) failed", px);
        return NULL;
    }

    s_cache[s_count].px   = px;
    s_cache[s_count].font = font;
    s_count++;

    /* 打印 TTF 地址：上板可确认落在 0x60xxxxxx(外部 QSPI flash) */
    LOG_I("cn font ready px=%d (ttf=%u bytes @ %p)",
          px, (unsigned)noto_gb2312_ttf_size, (const void *)noto_gb2312_ttf);
    return font;
}
