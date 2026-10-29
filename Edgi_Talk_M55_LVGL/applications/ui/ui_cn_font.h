/*
 * ui_cn_font.h — 中文 Tiny TTF 字体(外部 flash 的 GB2312 全集 TTF 运行时栅格化)
 *
 * 用途：让 LVGL 显示任意 GB2312 汉字(如云端 LLM 生成的健康建议)，
 * 不再受限于 lv_font_conv 预栅格化的静态子集(漏字=空白方块)。
 *
 * 字体数据 noto_gb2312_ttf[] 是 const 数组，落 .rodata -> 外部 QSPI flash，
 * 不占内部 SRAM；只有实际显示的字形栅格化后缓存进 heap。
 */
#ifndef UI_CN_FONT_H
#define UI_CN_FONT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取指定像素字号的中文字体
 *
 * 懒创建 + 按字号缓存：同一字号重复调用返回同一 lv_font_t，
 * 不同字号各创建一个(共享同一份外部 flash TTF，矢量缩放)。
 *
 * @param px 字号(像素)，需 > 0
 * @return 字体对象；失败返回 NULL(调用方应回退内置字体)
 */
lv_font_t *ui_cn_font_get(int px);

#ifdef __cplusplus
}
#endif

#endif /* UI_CN_FONT_H */
