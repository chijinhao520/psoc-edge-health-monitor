/*
 * advice_local.h — 设备端健康建议生成（离线，不依赖云端）
 *
 * 背景：原设计（task 07-15 的 03-mqtt-advice-subscribe）是
 *   M55 --MQTT--> 云端 LLM --MQTT--> M55 显示
 * 三个前提缺一不可：现场有 2.4G WiFi、MQTT broker 在线、LLM 服务可用。
 * 答辩现场任一环断掉，AI 建议卡片就永远停在"正在等待云端健康分析"。
 * 且把 AI 放云端本就弱化 Edge AI 竞赛主题（见 memory 2026-socc-voice-interaction
 * 的四路线评估：云端对话当"联网增值"不当主线）。
 *
 * 本模块在设备端**离线**生成建议正文，按键触发，与云端路径并存（谁先来显示谁）。
 *
 * ⚠ 这是**规则引擎，不是 LLM**，措辞上不要冒充"大模型生成"。
 * 但它的输入里包含 NPU 推理结果（心律不齐分类 / 血糖 CNN），
 * 说"设备端 Edge AI 评估"是站得住的 —— 结论来自板上算力，不是云。
 *
 * 与 health_report.c 的 advice[] 的区别：
 *   - advice[]  : 独立短句列表（"心率偏快，注意休息。"），已在报告页列表区显示
 *   - 本模块    : 把所有指标 + 评分 + NPU 结论组织成**一段连贯的话**，填 AI 卡片
 * 两者数据源相同、结论必须一致（本模块复用 health_report_eval 的判级，
 * 不另起一套阈值 —— 否则同屏两处会互相打脸）。
 */

#ifndef __ADVICE_LOCAL_H__
#define __ADVICE_LOCAL_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 生成正文所需的缓冲区下限。低于此值 advice_local_generate() 直接拒绝生成
 * （返回 0 + 置空串），**不再"能写多少写多少"**。
 *
 * ★ 512 → 1024（2026-07-17，对抗审 memory lens 抓出）：
 * 原值 512 与下面文档写的"建议 >= 1024"自相矛盾 —— 调用方给 512 时
 * `cap < 512` 判不出 `cap == 512`，于是"低于推荐值但达到硬下限"被无声放行，
 * 结果正文被静默截断、**医疗免责声明整句消失**（详见 ui_tab_report.c 的
 * AI_ADVICE_BUF_LEN 注释）。
 *
 * 教训：**文档写的下限和代码判的下限必须是同一个数**，否则文档就是摆设。
 * 实测正文常规约 900B、数据齐全时更多，故下限取 1024、调用方给 1536。
 */
#define ADVICE_LOCAL_MIN_BUF   1024

/**
 * @brief 用当前测量快照生成一段中文健康建议正文（UTF-8）。
 *
 * 纯字符串拼接 + 规则判定，无阻塞、无 IO，可直接在 LVGL 按键回调里调。
 * 数据取自 ui_data_get()，各项独立判 valid —— 没测到的指标整句跳过，
 * 绝不用 0 或残值编造结论。
 *
 * @param buf  输出缓冲；**必须 >= ADVICE_LOCAL_MIN_BUF(1024)**，否则拒绝生成。
 *             实测正文常规约 900 字节(中文 UTF-8 一字 3 字节)，数据齐全时更多，
 *             推荐直接给 1536（= 现在 AI_ADVICE_BUF_LEN 的值）。
 * @param cap  缓冲容量
 * @return 写入的字节数（不含结尾 '\0'）；cap 不足时返回 0 且 buf 置空串
 */
int advice_local_generate(char *buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* __ADVICE_LOCAL_H__ */
