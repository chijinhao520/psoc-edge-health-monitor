/*
 * mqtt_upload.c — M55 健康数据 MQTT 上传
 *
 * 线程 "mqtt_up"：等 WiFi 连通（手动连接，见 wifi_connect.c）后，
 * 连接公共测试 broker（localhost:1883，明文 QoS0），
 * 每 MQTT_UPLOAD_PERIOD_MS 从 ui_data_copy() 取一份健康数据快照，
 * 拼成 JSON 发布到 socc/health/<device_id>/telemetry，
 * 由 dashboard/server.py 订阅后存入本地数据库 + 网页展示。
 *
 * lwIP MQTT client 走 raw API，LWIP_TCPIP_CORE_LOCKING=1 (lwipopts.h)
 * 时从本线程(非 tcpip 线程)调用必须 LOCK_TCPIP_CORE()/UNLOCK_TCPIP_CORE()。
 *
 * 本文件所有浮点转字符串都手动拆整数/两位小数——本项目 newlib nano 不支持
 * rt_snprintf("%f", ...)（memory 2026-socc-lvgl-m55.md 已踩过这个坑）。
 */

#include <rtthread.h>
#include <board.h>
#include "mqtt_upload.h"
#include "wifi_connect.h"
#include "ui/ui_data.h"
#include "ui/ui_tab_report.h"   /* ui_report_set_advice() 下行建议刷 UI */

#include "lwip/apps/mqtt.h"
#include "lwip/tcpip.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"

#include "cy_pdl.h"

#define DBG_TAG "mqtt_up"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* ------------------------------------------------------------------ */
/*  配置                                                                */
/* ------------------------------------------------------------------ */

#define MQTT_UPLOAD_THREAD_STACK    4096
#define MQTT_UPLOAD_THREAD_PRIO     21
#define MQTT_UPLOAD_WIFI_POLL_MS    2000    /* 等 WiFi 连通的轮询间隔 */
#define MQTT_UPLOAD_CONNECT_WAIT_MS 8000    /* 等 MQTT CONNACK 的超时 */
#define MQTT_UPLOAD_RETRY_DELAY_MS  5000    /* 连接失败重试间隔 */
#define MQTT_UPLOAD_PERIOD_MS       10000   /* 数据发布周期 */

#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST  "localhost"  /* Configure your own trusted broker. */
#endif
#define MQTT_BROKER_PORT   1883

#define MQTT_DEVICE_ID_LEN   17    /* 16 hex + '\0' */
#define MQTT_TOPIC_LEN       64
#define MQTT_PAYLOAD_LEN     1024

/* ------------------------------------------------------------------ */
/*  状态                                                                */
/* ------------------------------------------------------------------ */

static mqtt_client_t     *s_client = RT_NULL;
static char                s_device_id[MQTT_DEVICE_ID_LEN];
static char                s_topic[MQTT_TOPIC_LEN];         /* 上行遥测 topic */
static char                s_advice_topic[MQTT_TOPIC_LEN];  /* 下行 AI 建议 topic */
static char                s_advice_req_topic[MQTT_TOPIC_LEN]; /* 上行建议请求 */
static volatile rt_bool_t  s_mqtt_connected = RT_FALSE;

/* 按键请求云端建议：LVGL 回调 release 信号量唤醒上传线程（LVGL 线程
 * 不碰 lwip）；pending 标志在连接断开期间保留请求，连上即发 */
static rt_sem_t            s_req_sem = RT_NULL;
static volatile int        s_req_pending = 0;

/* 下行 AI 建议接收缓冲(lwIP incoming data_cb 逐片重组)。
 * 1024：server 设备版建议 ≤150 中文字 ≈ 470B，原 512 贴着上限，
 * LLM 偶尔超写就截断 —— 与 ui_tab_report 的 1536 缓冲留同阶余量 */
#define ADVICE_RX_MAX 1024
static char     s_advice_rx[ADVICE_RX_MAX];
static uint16_t s_advice_rx_len = 0;

/* ------------------------------------------------------------------ */
/*  浮点→"整数.两位小数"辅助（规避 newlib nano 无 %f 支持）              */
/* ------------------------------------------------------------------ */

static void fmt2(float v, int *ip, int *fp)
{
    int scaled = (int)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
    if (scaled < 0) scaled=-scaled;
    *ip = scaled / 100;
    *fp = scaled % 100;
    if (*fp < 0)
        *fp = -*fp;
}

#define APPEND_JSON(buf, off, cap, ...) do {                          \
        if ((off) < 0 || (off) >= (cap)) return -1;                   \
        int _written = rt_snprintf((buf) + (off), (cap) - (off),     \
                                   __VA_ARGS__);                    \
        if (_written < 0 || _written >= (cap) - (off)) return -1;    \
        (off) += _written;                                          \
    } while (0)

#define APPEND_F2(buf, off, cap, key, val, sep) do {                  \
        int _fi, _ff;                                                 \
        fmt2((float)(val), &_fi, &_ff);                                \
        APPEND_JSON(buf, off, cap,                                  \
                              "\"" key "\":%s%d.%02d" sep,             \
                              (val)<0 ? "-" : "", _fi, _ff);         \
    } while (0)

/* ------------------------------------------------------------------ */
/*  设备标识（芯片唯一 ID，用于 topic + JSON device_id 字段）             */
/* ------------------------------------------------------------------ */

static void device_id_init(void)
{
    uint64_t uid = Cy_SysLib_GetUniqueId();
    uint32_t hi = (uint32_t)(uid >> 32);
    uint32_t lo = (uint32_t)(uid & 0xFFFFFFFFu);

    rt_snprintf(s_device_id, sizeof(s_device_id), "%08x%08x", hi, lo);
    rt_snprintf(s_topic, sizeof(s_topic), "socc/health/%s/telemetry", s_device_id);
    rt_snprintf(s_advice_topic, sizeof(s_advice_topic), "socc/health/%s/advice", s_device_id);
    rt_snprintf(s_advice_req_topic, sizeof(s_advice_req_topic),
                "socc/health/%s/advice_req", s_device_id);
}

/* ------------------------------------------------------------------ */
/*  云端建议请求 API（见 mqtt_upload.h）                                 */
/* ------------------------------------------------------------------ */

rt_bool_t mqtt_advice_ready(void)
{
    return (wifi_is_connected() && s_mqtt_connected) ? RT_TRUE : RT_FALSE;
}

void mqtt_advice_request(void)
{
    /* 连点合并：已有在途请求就不再 release（本函数只被 LVGL 按键回调
     * 单线程调用，读改写无并发；消费侧另有 sem 排空兜底） */
    if (s_req_pending)
        return;
    s_req_pending = 1;
    if (s_req_sem != RT_NULL)
        rt_sem_release(s_req_sem);
}

/* ------------------------------------------------------------------ */
/*  MQTT 回调                                                           */
/* ------------------------------------------------------------------ */

/* ---- 下行 AI 建议: incoming 分片重组 ---- */
static void advice_inpub_cb(void *arg, const char *topic, u32_t tot_len)
{
    RT_UNUSED(arg); RT_UNUSED(topic); RT_UNUSED(tot_len);
    s_advice_rx_len = 0;    /* 新消息开始, 重置重组缓冲 */
}

static void advice_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    RT_UNUSED(arg);
    /* 逐片追加(带截断保护, 留 1 字节给结尾 '\0') */
    if (s_advice_rx_len < ADVICE_RX_MAX - 1) {
        uint16_t room = (uint16_t)((ADVICE_RX_MAX - 1) - s_advice_rx_len);
        uint16_t n = (len < room) ? len : room;
        rt_memcpy(s_advice_rx + s_advice_rx_len, data, n);
        s_advice_rx_len = (uint16_t)(s_advice_rx_len + n);
    }
    if (flags & MQTT_DATA_FLAG_LAST) {
        s_advice_rx[s_advice_rx_len] = '\0';
        ui_report_set_advice(s_advice_rx);   /* 跨线程安全: 内部只置脏, 不碰 lv_* */
        LOG_I("advice recv %u bytes", (unsigned)s_advice_rx_len);
    }
}

static void advice_sub_cb(void *arg, err_t err)
{
    RT_UNUSED(arg);
    if (err == ERR_OK) LOG_I("advice subscribed");
    else               LOG_W("advice subscribe failed, err=%d", (int)err);
}

static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    RT_UNUSED(arg);

    if (status == MQTT_CONNECT_ACCEPTED)
    {
        s_mqtt_connected = RT_TRUE;
        LOG_I("connected to broker %s:%d", MQTT_BROKER_HOST, MQTT_BROKER_PORT);

        /* 连上后订阅下行 AI 建议 topic(在 tcpip 回调上下文, 直接调无需额外锁) */
        mqtt_set_inpub_callback(client, advice_inpub_cb, advice_data_cb, RT_NULL);
        err_t serr = mqtt_subscribe(client, s_advice_topic, 0, advice_sub_cb, RT_NULL);
        if (serr != ERR_OK)
            LOG_W("mqtt_subscribe(advice) call failed, err=%d", (int)serr);
        else
            LOG_I("subscribing advice topic: %s", s_advice_topic);
    }
    else
    {
        s_mqtt_connected = RT_FALSE;
        LOG_W("broker disconnected/refused, status=%d", (int)status);
    }
}

static void mqtt_pub_request_cb(void *arg, err_t err)
{
    RT_UNUSED(arg);
    if (err != ERR_OK)
        LOG_W("publish request failed, err=%d", (int)err);
}

/* ------------------------------------------------------------------ */
/*  建连                                                                */
/* ------------------------------------------------------------------ */

static rt_bool_t mqtt_do_connect(void)
{
    struct hostent *he = lwip_gethostbyname(MQTT_BROKER_HOST);
    if (he == RT_NULL || he->h_addr_list == RT_NULL || he->h_addr_list[0] == RT_NULL)
    {
        LOG_W("DNS resolve \"%s\" failed", MQTT_BROKER_HOST);
        return RT_FALSE;
    }

    ip_addr_t broker_addr;
    const uint8_t *raw = (const uint8_t *)he->h_addr_list[0];
    IP4_ADDR(ip_2_ip4(&broker_addr), raw[0], raw[1], raw[2], raw[3]);
#if LWIP_IPV4 && LWIP_IPV6
    IP_SET_TYPE_VAL(broker_addr, IPADDR_TYPE_V4);
#endif

    struct mqtt_connect_client_info_t ci;
    rt_memset(&ci, 0, sizeof(ci));
    ci.client_id  = s_device_id;
    ci.keep_alive = 60;

    LOCK_TCPIP_CORE();
    err_t err = mqtt_client_connect(s_client, &broker_addr, MQTT_BROKER_PORT,
                                     mqtt_connection_cb, RT_NULL, &ci);
    UNLOCK_TCPIP_CORE();

    if (err != ERR_OK)
    {
        LOG_W("mqtt_client_connect() failed, err=%d", (int)err);
        return RT_FALSE;
    }
    return RT_TRUE;
}

/* ------------------------------------------------------------------ */
/*  组装 JSON payload                                                   */
/* ------------------------------------------------------------------ */

static int build_payload(char *buf, int cap)
{
    struct ui_health_data snapshot;
    ui_data_copy(&snapshot);
    const struct ui_health_data *d = &snapshot;
    int off = 0;
    if (buf == RT_NULL || cap <= 0) return -1;

    APPEND_JSON(buf, off, cap,
                        "{\"device_id\":\"%s\",\"heart_rate\":%d,",
                        s_device_id, d->heart_rate);

    APPEND_F2(buf, off, cap, "spo2",          d->spo2,          ",");
    APPEND_F2(buf, off, cap, "body_temp",     d->body_temp,     ",");
    APPEND_F2(buf, off, cap, "glucose",       d->glucose,       ",");
    /* 每项指标的有效标志随值上传，云端据此排除未测和失效数据。 */
    APPEND_JSON(buf, off, cap,
                        "\"glucose_valid\":%d,", d->glucose_valid);
    APPEND_F2(buf, off, cap, "bmr",           d->bmr,           ",");
    APPEND_F2(buf, off, cap, "env_temp",      d->env_temp,      ",");
    APPEND_F2(buf, off, cap, "env_humidity",  d->env_humidity,  ",");
    APPEND_F2(buf, off, cap, "blood_flow",    d->blood_flow,    ",");

    APPEND_JSON(buf, off, cap,
                        "\"arr_class\":%d,\"arr_valid\":%d,"
                        "\"arr_beat_count\":%u,\"arr_pvc_per_min\":%u,"
                        "\"arr_source\":%d,\"arr_pvc_valid\":%d,",
                        d->arr_class, d->arr_valid,
                        d->arr_beat_count, d->arr_pvc_per_min,
                        d->arr_source, d->arr_pvc_valid);

    APPEND_F2(buf, off, cap, "hrv_rmssd_ms",  d->hrv_rmssd_ms,  ",");
    APPEND_JSON(buf, off, cap, "\"hrv_valid\":%d,", d->hrv_valid);

    APPEND_F2(buf, off, cap, "resp_rate_bpm", d->resp_rate_bpm, ",");
    APPEND_JSON(buf, off, cap,
                        "\"resp_valid\":%d,\"health_score\":%d,\"health_score_valid\":%d,",
                        d->resp_valid, d->health_score, d->health_score_valid);

    APPEND_JSON(buf, off, cap,
        "\"hr_valid\":%d,\"spo2_valid\":%d,\"body_temp_valid\":%d,"
        "\"bmr_valid\":%d,\"env_valid\":%d,\"blood_flow_valid\":0}",
        d->hr_valid,d->spo2_valid,d->body_temp_valid,d->bmr_valid,d->env_valid);
    return off;
}

/* ------------------------------------------------------------------ */
/*  线程入口                                                            */
/* ------------------------------------------------------------------ */

static void mqtt_upload_thread_entry(void *param)
{
    RT_UNUSED(param);

    device_id_init();
    LOG_I("device_id=%s topic=%s", s_device_id, s_topic);

    s_client = mqtt_client_new();
    if (s_client == RT_NULL)
    {
        LOG_E("mqtt_client_new() failed, upload disabled");
        return;
    }

    while (1)
    {
        if (!wifi_is_connected())
        {
            s_mqtt_connected = RT_FALSE;
            rt_thread_mdelay(MQTT_UPLOAD_WIFI_POLL_MS);
            continue;
        }

        if (!mqtt_client_is_connected(s_client))
        {
            if (!mqtt_do_connect())
            {
                rt_thread_mdelay(MQTT_UPLOAD_RETRY_DELAY_MS);
                continue;
            }

            int waited = 0;
            while (!s_mqtt_connected && waited < MQTT_UPLOAD_CONNECT_WAIT_MS)
            {
                rt_thread_mdelay(200);
                waited += 200;
            }
            if (!s_mqtt_connected)
            {
                LOG_W("connect timeout");
                rt_thread_mdelay(MQTT_UPLOAD_RETRY_DELAY_MS);
                continue;
            }
        }

        char payload[MQTT_PAYLOAD_LEN];
        int len = build_payload(payload, sizeof(payload));
        if (len <= 0 || len >= (int)sizeof(payload)) {
            LOG_W("telemetry payload could not be encoded");
            rt_thread_mdelay(MQTT_UPLOAD_PERIOD_MS);
            continue;
        }

        LOCK_TCPIP_CORE();
        err_t err = mqtt_publish(s_client, s_topic, payload, (u16_t)len,
                                  0 /* QoS0 */, 0 /* retain */,
                                  mqtt_pub_request_cb, RT_NULL);
        UNLOCK_TCPIP_CORE();

        if (err != ERR_OK)
            LOG_W("mqtt_publish() failed, err=%d", (int)err);
        else
            LOG_D("published %d bytes -> %s", len, s_topic);

        /* 按键请求云端建议：payload 与 telemetry 同构（自带最新快照，
         * server 直接入库+立即生成，不依赖上面那条恰好先到）。
         * pending 只在发布**成功**后清零 —— 失败（ERR_MEM/ERR_CONN）保留
         * 请求，下轮（≤10s）自动重发，直到发出为止；否则 UI 上那句
         * "已请求云端"就是在对用户撒谎（对抗审 finding①） */
        if (s_req_pending)
        {
            LOCK_TCPIP_CORE();
            err = mqtt_publish(s_client, s_advice_req_topic, payload, (u16_t)len,
                               0, 0, mqtt_pub_request_cb, RT_NULL);
            UNLOCK_TCPIP_CORE();
            if (err != ERR_OK)
            {
                LOG_W("advice_req publish failed (err=%d), retry next cycle",
                      (int)err);
            }
            else
            {
                s_req_pending = 0;
                LOG_I("advice request sent -> %s", s_advice_req_topic);
            }
        }

        /* 10s 例行周期；按键请求经 sem 立刻唤醒（回到循环头重走
         * 连接检查后发布）。sem 超时 = 正常节拍，非错误。
         * 醒来后把残余计数一次排空：快速连点累积的 N 个 release 合并为
         * 一轮处理，不绕过 10s 节流（对抗审 finding②） */
        if (s_req_sem != RT_NULL)
        {
            rt_sem_take(s_req_sem, rt_tick_from_millisecond(MQTT_UPLOAD_PERIOD_MS));
            while (rt_sem_take(s_req_sem, 0) == RT_EOK) { /* drain */ }
        }
        else
        {
            rt_thread_mdelay(MQTT_UPLOAD_PERIOD_MS);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  初始化                                                              */
/* ------------------------------------------------------------------ */

static int mqtt_upload_init(void)
{
    s_req_sem = rt_sem_create("advreq", 0, RT_IPC_FLAG_PRIO);
    if (s_req_sem == RT_NULL)
        LOG_W("advice req sem create failed, button request degrades to 10s poll");

    rt_thread_t tid = rt_thread_create(
        "mqtt_up",
        mqtt_upload_thread_entry,
        RT_NULL,
        MQTT_UPLOAD_THREAD_STACK,
        MQTT_UPLOAD_THREAD_PRIO,
        10);

    if (tid == RT_NULL)
    {
        LOG_E("mqtt_up thread create failed");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    LOG_I("mqtt_up task created (prio=%d, stack=%d)",
          MQTT_UPLOAD_THREAD_PRIO, MQTT_UPLOAD_THREAD_STACK);
    return RT_EOK;
}
INIT_APP_EXPORT(mqtt_upload_init);
