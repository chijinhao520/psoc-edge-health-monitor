/*
 * advice_local.c — 设备端健康建议生成（说明见 advice_local.h）
 *
 * 正文用 tiny_ttf(GB2312 全集)显示，不受 ui_font_Subtitle 那 365 字子集限制，
 * 故此处中文直接写字面量、不必用 \x 转义。
 * （AI 卡片的 label 绑的就是 ui_cn_font_get(18)，见 ui_tab_report.c:381）
 */

#include <rtthread.h>
#include "advice_local.h"
#include "health_report.h"
#include "arrhythmia_task.h"   /* ARR_SRC_NPU：页脚"NPU 推理"措辞前必须核对来源 */
#include "ui/ui_data.h"
#include <stdio.h>
#include <string.h>

#define DBG_TAG "advice"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 往 buf 追加，自动防溢出。pos 为已写长度，返回新长度 */
static int app(char *buf, int cap, int pos, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (pos >= cap - 1)
        return pos;
    va_start(ap, fmt);
    n = rt_vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0)
        return pos;
    pos += n;
    return (pos > cap - 1) ? (cap - 1) : pos;
}

int advice_local_generate(char *buf, int cap)
{
    const struct ui_health_data *d;
    health_report_input_t in;
    health_report_t r;
    int pos = 0;
    int i;

    if (buf == RT_NULL || cap < ADVICE_LOCAL_MIN_BUF)
    {
        if (buf && cap > 0)
            buf[0] = '\0';
        return 0;
    }
    buf[0] = '\0';

    d = ui_data_get();
    if (d == RT_NULL)
        return 0;

    /*
     * 复用 health_report_build 的判级，不另起一套阈值 ——
     * 报告页的指标列表和本卡片同屏显示，两处结论必须一致，
     * 否则列表说"心率正常"、卡片说"心率偏快"，直接自打脸。
     */
    rt_memset(&in, 0, sizeof(in));
    in.heart_rate      = d->heart_rate;      in.hr_valid        = d->hr_valid;
    in.spo2            = d->spo2;            in.spo2_valid      = d->spo2_valid;
    in.arr_valid       = d->arr_valid && d->arr_pvc_valid; in.arr_class = d->arr_class;
    in.arr_beat_count  = d->arr_beat_count;  in.arr_pvc_per_min = d->arr_pvc_per_min;
    in.hrv_sdnn_ms     = d->hrv_sdnn_ms;     in.hrv_valid       = d->hrv_valid;
    in.resp_rate_bpm   = d->resp_rate_bpm;   in.resp_valid      = d->resp_valid;
    in.gsr_stress      = d->gsr_stress_index;in.gsr_valid       = d->gsr_valid;
    in.glucose_mmol    = d->glucose;         in.glucose_valid   = d->glucose_valid;
    in.health_score    = d->health_score;    in.health_score_valid = d->health_score_valid;
    health_report_build(&in, &r);

    /* ---- 开篇：综合评分 ---- */
    if (r.score_valid)
        pos = app(buf, cap, pos, "本次体检综合评分 %d 分，整体状态%s。\n\n",
                  r.score, r.grade_text);
    else
        pos = app(buf, cap, pos, "本次体检尚未取得完整数据，以下为已测项目的评估。\n\n");

    /* ---- 生命体征：把测到的项串成一句话 ---- */
    {
        int any = 0;
        pos = app(buf, cap, pos, "【生命体征】");
        if (d->hr_valid)
        {
            pos = app(buf, cap, pos, "心率 %d 次/分，%s。",
                      d->heart_rate, r.m[HRPT_M_HR].status_text);
            any = 1;
        }
        if (d->spo2_valid)
        {
            pos = app(buf, cap, pos, "血氧饱和度 %d%%，%s。",
                      (int)(d->spo2 + 0.5f), r.m[HRPT_M_SPO2].status_text);
            any = 1;
        }
        if (d->body_temp_valid)
        {
            pos = app(buf, cap, pos, "体温 %d.%d ℃。",
                      (int)d->body_temp,
                      (int)((d->body_temp - (int)d->body_temp) * 10.0f + 0.5f));
            any = 1;
        }
        if (!any)
            pos = app(buf, cap, pos, "尚未测到有效数据，请将手指稳定放置于传感器后重新测量。");
        pos = app(buf, cap, pos, "\n\n");
    }

    /* ---- 心脏电活动：NPU 推理结论。这是"Edge AI"的实处 —— 分类由板上 NPU 出 ---- */
    if (d->arr_valid)
    {
        pos = app(buf, cap, pos, "【心律分析】已分析 %u 个心拍，%s。",
                  d->arr_beat_count, r.m[HRPT_M_RHYTHM].status_text);
        if (d->arr_pvc_valid && d->arr_pvc_per_min > 0)
            pos = app(buf, cap, pos, "近一分钟检出室性早搏 %u 次。", d->arr_pvc_per_min);
        pos = app(buf, cap, pos, "\n\n");
    }

    /* ---- 自主神经：HRV + 呼吸 + 应激 ---- */
    {
        int any = 0;
        pos = app(buf, cap, pos, "【自主神经与状态】");
        if (d->hrv_valid)
        {
            pos = app(buf, cap, pos, "心率变异性 %d 毫秒，%s。",
                      (int)(d->hrv_sdnn_ms + 0.5f), r.m[HRPT_M_HRV].status_text);
            any = 1;
        }
        if (d->resp_valid)
        {
            pos = app(buf, cap, pos, "呼吸 %d 次/分，%s。",
                      (int)(d->resp_rate_bpm + 0.5f), r.m[HRPT_M_RESP].status_text);
            any = 1;
        }
        if (d->gsr_valid)
        {
            pos = app(buf, cap, pos, "应激指数 %d，%s。",
                      (int)(d->gsr_stress_index + 0.5f), r.m[HRPT_M_STRESS].status_text);
            any = 1;
        }
        if (!any)
            pos = app(buf, cap, pos, "需佩戴心电电极并静坐约 30 秒方可评估。");
        pos = app(buf, cap, pos, "\n\n");
    }

    /* ---- 建议：直接复用 health_report 排好序的句子 ---- */
    if (r.advice_count > 0)
    {
        pos = app(buf, cap, pos, "【建议】\n");
        for (i = 0; i < r.advice_count; i++)
            pos = app(buf, cap, pos, "%d. %s\n", i + 1, r.advice[i]);
        pos = app(buf, cap, pos, "\n");
    }

    /*
     * 页脚：口径必须诚实。
     * 本模块是规则引擎不是 LLM。措辞注意（2026-07-17 实测踩坑）：
     * 曾写"未经联网"——本意是"这份报告的生成没走云端"，但用户联网后
     * 按键看到这句，以为 WiFi 没连上/功能坏了。改为说清两条路径：
     * 按键=离线版立即出，云端版=联网后自动推送覆盖（谁最后写谁显示，
     * 见 mqtt_upload.c advice_data_cb）。
     * "NPU 推理得出"只在最近一拍确实走了 NPU（L1）时才能写：
     * 走 L3 规则兜底时这句话是假的，评委追问一句就穿帮。
     * 血糖单独再标一次实验性：它是 trivial-mean 预测器
     * （见 memory 2026-socc-glucose-model），出现在报告里必须让人知道不能当数。
     */
    pos = app(buf, cap, pos,
              "本报告由设备端离线引擎即时生成；联网后云端个性化分析将自动推送更新。");
    if (d->arr_valid && d->arr_source == ARR_SRC_NPU)
        pos = app(buf, cap, pos, "心律分类由板载 NPU 推理得出；");
    else if (d->arr_valid)
        pos = app(buf, cap, pos, "心律分类由设备端规则算法得出；");
    pos = app(buf, cap, pos, "呼吸率与血糖为实验性指标，仅供参考。");
    pos = app(buf, cap, pos, "\n%s", r.disclaimer ? r.disclaimer : "");

    LOG_I("local advice generated: %d bytes", pos);
    return pos;
}
