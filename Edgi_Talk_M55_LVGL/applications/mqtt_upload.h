/*
 * mqtt_upload.h — M55 健康数据 MQTT 上传 + 云端建议请求
 *
 * 上行：周期性把 ui_data_get() 快照发布到 socc/health/<device_id>/telemetry。
 * 下行：订阅 socc/health/<device_id>/advice，云端建议到达后经
 *       ui_report_set_advice() 刷新报告页 AI 卡片。
 * 请求：mqtt_advice_request() 唤醒上传线程，立即发一份最新快照到
 *       .../advice_req —— 服务器收到后即时调 LLM 生成并推回（不等
 *       180s 被动轮询）。按键"开始分析"联网时走这条路。
 */

#ifndef __MQTT_UPLOAD_H__
#define __MQTT_UPLOAD_H__

#include <rtthread.h>

/**
 * @brief WiFi + MQTT broker 是否都已连接（云端建议请求是否可达）。
 *
 * LVGL 按键回调里判断用。TRUE 只代表链路在，不保证服务器/LLM 活着 ——
 * 云端版是否真回来以 advice topic 的到达为准（到达即覆盖 AI 卡片）。
 */
rt_bool_t mqtt_advice_ready(void);

/**
 * @brief 请求云端即时生成健康建议（非阻塞，可在 LVGL 回调直接调）。
 *
 * 只唤醒上传线程：由它在自己的上下文里 publish 最新 telemetry +
 * advice_req（LVGL 线程不碰 lwip）。未联网时调用无害（线程醒来后
 * 发现连接不在会走重连流程，请求标志保留到连上为止）。
 */
void mqtt_advice_request(void);

#endif /* __MQTT_UPLOAD_H__ */
