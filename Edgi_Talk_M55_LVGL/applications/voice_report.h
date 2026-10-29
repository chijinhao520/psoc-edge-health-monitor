/*
 * voice_report.h — 测量结果语音播报（T2）
 *
 * 建立在 T1 打通的 sound0 通路上（见 memory 2026-socc-voice-interaction）。
 * 策略：预录封闭集 PCM 片段（voice_data.c，SAPI 生成）+ 运行时按数值拼接。
 * MCU 上跑 TTS 模型太重，而播报内容是封闭集（数字 + 十几个固定词），拼接够用。
 *
 * 播报是"家用健康医生"定位最贴合的一环 —— 老人不用盯屏幕。
 *
 * ⚠ 数据可信性：本模块只播 valid 的值。没测到就不播，绝不播 0 或残值 ——
 * 屏幕上显示 "--" 用户一眼能看出没数，但语音说"心率零次每分"会被当成真结果。
 */

#ifndef __VOICE_REPORT_H__
#define __VOICE_REPORT_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播报一组测量结果："测量完成，心率 X 次每分，血氧百分之 Y，体温 Z 度"
 *
 * 各项独立判 valid，无效的整项跳过（不播 0）。全部无效时只播"测量完成"。
 * ⚠ 阻塞式，约 3~5 秒。**别在 LVGL 回调 / IPC 轮询里直接调** —— 那会卡死整个
 * UI 线程。要从那些地方触发请用 voice_report_request()。
 *
 * @param hr        心率 bpm；hr_valid=0 时跳过该项
 * @param spo2      血氧 %；spo2_valid=0 时跳过
 * @param temp_c    体温 °C；temp_valid=0 时跳过（播到小数点后一位）
 */
void voice_report_result(int hr, int hr_valid,
                         int spo2, int spo2_valid,
                         float temp_c, int temp_valid);

/**
 * @brief 请求播报（**非阻塞**，安全用于 LVGL 回调 / IPC 轮询）。
 *
 * 只是唤醒播报线程就返回。线程会先等测量结果真正落到 IPC（M33 的 bmr_task
 * 是 5s 周期，测量会话报 DONE 的那一刻 IPC 里的 hr/spo2 往往还是上一轮的），
 * 再读当前值播报。重复请求在播报进行中会被忽略。
 */
void voice_report_request(void);

/** @brief 只播"测量完成"（快速反馈，约 1 秒）。 */
void voice_report_done(void);

/**
 * @brief 播一个 0~999 的整数（内部用，导出便于单测/诊断）。
 * @return 0 成功
 */
int voice_say_number(int n);

#ifdef __cplusplus
}
#endif

#endif /* __VOICE_REPORT_H__ */
