/*
 * WiFi 自动连接 + 扫描功能
 * - 启动后自动连接预设 AP
 * - 提供扫描 API 供 LVGL UI 调用
 */
#include <rtthread.h>
#include "wifi_connect.h"

#ifdef RT_USING_WIFI

#include <rtdevice.h>
#include <wlan_mgnt.h>
#include <wlan_prot.h>
#include <wlan_dev.h>
#include <lwip/inet.h>
#include <netdev.h>
#include <string.h>

/* ======== 预设 WiFi 凭据 ========
 * 后续改为 LVGL UI 触摸键盘输入，或从 Flash 读取 */
#ifndef WIFI_SSID
#define WIFI_SSID      ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD   ""
#endif

#define WIFI_CONNECT_TIMEOUT_MS   15000   /* 连接超时 */
#define WIFI_DHCP_WAIT_MS          8000   /* DHCP 等待 */
#define WIFI_RETRY_DELAY_MS        3000   /* 重连间隔 */
#define WIFI_MAX_RETRY             5      /* 最大重试次数 */

/* ======== 连接状态 ======== */

static volatile rt_bool_t _wifi_connected = RT_FALSE;
static char _connected_ssid[WIFI_SSID_MAX_LEN] = {0};
static char _ip_str[16] = {0};

rt_bool_t wifi_is_connected(void)
{
    return _wifi_connected;
}

const char *wifi_get_connected_ssid(void)
{
    return _wifi_connected ? _connected_ssid : RT_NULL;
}

const char *wifi_get_ip_str(void)
{
    return _wifi_connected ? _ip_str : RT_NULL;
}

static void _wifi_ready_handler(int event, struct rt_wlan_buff *buff, void *param)
{
    (void)buff;
    (void)param;
    _wifi_connected = RT_TRUE;

    /* 记录 SSID */
    struct netdev *ndev = netdev_get_by_name(RT_WLAN_DEVICE_STA_NAME);
    if (ndev)
    {
        rt_strncpy(_ip_str, inet_ntoa(ndev->ip_addr), sizeof(_ip_str) - 1);
    }

    rt_kprintf("[WiFi] network ready\n");
}

static void _wifi_disconnect_handler(int event, struct rt_wlan_buff *buff, void *param)
{
    (void)buff;
    (void)param;
    _wifi_connected = RT_FALSE;
    _connected_ssid[0] = '\0';
    _ip_str[0] = '\0';
    rt_kprintf("[WiFi] disconnected\n");
}

/* ======== 事件回调注册(幂等，手动连接/自动连接共用) ======== */

static rt_bool_t _handlers_registered = RT_FALSE;

static void ensure_event_handlers_registered(void)
{
    if (_handlers_registered)
        return;
    rt_wlan_register_event_handler(RT_WLAN_EVT_READY,
                                   _wifi_ready_handler, RT_NULL);
    rt_wlan_register_event_handler(RT_WLAN_EVT_STA_DISCONNECTED,
                                   _wifi_disconnect_handler, RT_NULL);
    _handlers_registered = RT_TRUE;
}

/* 事件回调本身不需要射频就绪，注册本身是安全的，开机就挂上，
 * 不用等 4s——真正需要等待的是下面 scan/connect 那些主动操作。 */
static int wifi_events_init(void)
{
    ensure_event_handlers_registered();
    return 0;
}
INIT_APP_EXPORT(wifi_events_init);

/* ======== 自动连接 ======== */

static void wifi_connect_thread(void *param)
{
    (void)param;
    rt_err_t ret;
    int retry = 0;

    /* 等待 WHD 驱动初始化完成（WHD ~2.5s 注册，射频需更长时间就绪） */
    rt_thread_mdelay(4000);

    ensure_event_handlers_registered();

    while (retry < WIFI_MAX_RETRY)
    {
        rt_kprintf("[WiFi] connecting to \"%s\" (attempt %d/%d)...\n",
                   WIFI_SSID, retry + 1, WIFI_MAX_RETRY);

        ret = rt_wlan_connect(WIFI_SSID, WIFI_PASSWORD);
        if (ret != RT_EOK)
        {
            rt_kprintf("[WiFi] connect failed: %d\n", ret);
            retry++;
            if (retry < WIFI_MAX_RETRY)
                rt_thread_mdelay(WIFI_RETRY_DELAY_MS);
            continue;
        }

        rt_kprintf("[WiFi] associated, waiting for DHCP...\n");

        /* 等待 DHCP 分配 IP */
        int wait = 0;
        while (wait < WIFI_DHCP_WAIT_MS)
        {
            if (_wifi_connected)
                break;
            rt_thread_mdelay(500);
            wait += 500;
        }

        if (_wifi_connected)
        {
            rt_strncpy(_connected_ssid, WIFI_SSID, WIFI_SSID_MAX_LEN - 1);
            struct netdev *ndev = netdev_get_by_name(RT_WLAN_DEVICE_STA_NAME);
            if (ndev)
            {
                rt_strncpy(_ip_str, inet_ntoa(ndev->ip_addr), sizeof(_ip_str) - 1);
                rt_kprintf("[WiFi] IP: %s\n", _ip_str);
            }
            rt_kprintf("[WiFi] ready!\n");
            break;
        }
        else
        {
            rt_kprintf("[WiFi] DHCP timeout\n");
            rt_wlan_disconnect();
            retry++;
            if (retry < WIFI_MAX_RETRY)
                rt_thread_mdelay(WIFI_RETRY_DELAY_MS);
        }
    }

    if (!_wifi_connected)
    {
        rt_kprintf("[WiFi] all attempts failed, giving up\n");
    }
}

static int wifi_auto_connect_init(void)
{
    rt_thread_t tid = rt_thread_create("wifi_conn",
                                       wifi_connect_thread, RT_NULL,
                                       4096, 20, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);

    return 0;
}
/* [诊断 2026-07-07] 二分定位到:ui_tab_settings.c 引用 ui_wifi_panel_show() 会把
 * 本文件整个链接进来,INIT_APP_EXPORT 注册的函数被链接脚本 KEEP() 无条件保留、
 * 开机自动执行——即使用户从没点开过 WiFi 面板。这个线程只等 4s 就 rt_wlan_connect()，
 * 与 CLAUDE.md 记录的"射频~14s才就绪,WHD未就绪调用会崩溃"的已知坑高度吻合，且
 * 与项目已有的 WiFi 驱动上电时序可能冲突。先注释掉验证是不是它导致的卡死。
 * 详见 memory 2026-socc-m55-toolchain-regression / 2026-socc-wifi.md。 */
// INIT_APP_EXPORT(wifi_auto_connect_init);

/* ======== 手动连接(WiFi 面板点选 AP + 输入密码) ======== */

#define WIFI_MANUAL_PASS_LEN  65   /* WPA2 密码最长 63 字符 + '\0'，留余量 */

static char             _manual_ssid[WIFI_SSID_MAX_LEN];
static char             _manual_pass[WIFI_MANUAL_PASS_LEN];
static volatile int      _manual_status = WIFI_CONN_IDLE;

static void wifi_manual_connect_thread(void *param)
{
    (void)param;

    ensure_event_handlers_registered();

    rt_kprintf("[WiFi] manual connect to \"%s\"...\n", _manual_ssid);

    /* 已经连着别的网络就先断开，避免 rt_wlan_connect 在旧连接上状态混乱 */
    if (_wifi_connected)
    {
        rt_wlan_disconnect();
        rt_thread_mdelay(200);
    }

    rt_err_t ret = rt_wlan_connect(_manual_ssid, _manual_pass);
    if (ret != RT_EOK)
    {
        rt_kprintf("[WiFi] manual connect failed: %d\n", ret);
        _manual_status = WIFI_CONN_FAILED;
        return;
    }

    int wait = 0;
    while (wait < WIFI_DHCP_WAIT_MS)
    {
        if (_wifi_connected)
            break;
        rt_thread_mdelay(500);
        wait += 500;
    }

    if (_wifi_connected)
    {
        rt_strncpy(_connected_ssid, _manual_ssid, WIFI_SSID_MAX_LEN - 1);
        struct netdev *ndev = netdev_get_by_name(RT_WLAN_DEVICE_STA_NAME);
        if (ndev)
            rt_strncpy(_ip_str, inet_ntoa(ndev->ip_addr), sizeof(_ip_str) - 1);
        _manual_status = WIFI_CONN_SUCCESS;
        rt_kprintf("[WiFi] manual connect success, IP: %s\n", _ip_str);
    }
    else
    {
        rt_wlan_disconnect();
        _manual_status = WIFI_CONN_FAILED;
        rt_kprintf("[WiFi] manual connect DHCP timeout\n");
    }
}

void wifi_connect_to(const char *ssid, const char *password)
{
    if (ssid == RT_NULL || ssid[0] == '\0')
    {
        _manual_status = WIFI_CONN_FAILED;
        return;
    }

    rt_strncpy(_manual_ssid, ssid, WIFI_SSID_MAX_LEN - 1);
    _manual_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

    if (password != RT_NULL)
        rt_strncpy(_manual_pass, password, WIFI_MANUAL_PASS_LEN - 1);
    else
        _manual_pass[0] = '\0';
    _manual_pass[WIFI_MANUAL_PASS_LEN - 1] = '\0';

    _manual_status = WIFI_CONN_CONNECTING;

    rt_thread_t tid = rt_thread_create("wifi_mconn",
                                       wifi_manual_connect_thread, RT_NULL,
                                       4096, 20, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
    else
        _manual_status = WIFI_CONN_FAILED;
}

int wifi_manual_connect_status(void)
{
    return _manual_status;
}

/* ======== WiFi 扫描 ======== */

static wifi_scan_item_t  _scan_buf[WIFI_SCAN_MAX];
static volatile int      _scan_count  = 0;
static volatile int      _scan_state  = WIFI_SCAN_IDLE;

/* SCAN_REPORT 回调：每发现一个 AP 调用一次（在 wlan 工作线程中） */
static void _scan_report_cb(int event, struct rt_wlan_buff *buff, void *param)
{
    (void)param;

    if (event != RT_WLAN_EVT_SCAN_REPORT || buff == RT_NULL)
        return;

    struct rt_wlan_info *info = (struct rt_wlan_info *)buff->data;
    if (info == RT_NULL || info->ssid.len == 0)
        return;

    int idx = _scan_count;
    if (idx >= WIFI_SCAN_MAX)
        return;

    /* 去重：同 SSID 只保留信号最强的 */
    for (int i = 0; i < idx; i++)
    {
        if (rt_strcmp(_scan_buf[i].ssid, (const char *)info->ssid.val) == 0)
        {
            /* 已存在，保留 RSSI 更强的 */
            if (info->rssi > _scan_buf[i].rssi)
            {
                _scan_buf[i].rssi = info->rssi;
                _scan_buf[i].channel = (uint8_t)info->channel;
            }
            return;
        }
    }

    /* 新 AP */
    wifi_scan_item_t *item = &_scan_buf[idx];
    rt_memset(item, 0, sizeof(*item));
    rt_memcpy(item->ssid, info->ssid.val, info->ssid.len < WIFI_SSID_MAX_LEN ? info->ssid.len : WIFI_SSID_MAX_LEN - 1);
    item->rssi = info->rssi;
    item->has_password = (info->security != SECURITY_OPEN) ? 1 : 0;
    item->channel = (uint8_t)info->channel;

    _scan_count = idx + 1;
}

/* 扫描后台线程 */
static void wifi_scan_thread_entry(void *param)
{
    (void)param;

    _scan_count = 0;
    _scan_state = WIFI_SCAN_RUNNING;

    /* 注册回调 */
    rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_REPORT,
                                   _scan_report_cb, RT_NULL);

    /* 同步扫描（阻塞 ~10s） */
    rt_err_t ret = rt_wlan_scan_with_info(RT_NULL);

    /* 注销回调 */
    rt_wlan_unregister_event_handler(RT_WLAN_EVT_SCAN_REPORT);

    if (ret == RT_EOK)
    {
        _scan_state = WIFI_SCAN_DONE;
        rt_kprintf("[WiFi] scan done, found %d AP(s)\n", _scan_count);
    }
    else
    {
        _scan_state = WIFI_SCAN_FAILED;
        rt_kprintf("[WiFi] scan failed: %d\n", ret);
    }
}

void wifi_start_scan(void)
{
    if (_scan_state == WIFI_SCAN_RUNNING)
        return;  /* 已在扫描中 */

    rt_thread_t tid = rt_thread_create("wifi_scan",
                                       wifi_scan_thread_entry, RT_NULL,
                                       4096, 22, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
}

int wifi_scan_status(void)
{
    return _scan_state;
}

int wifi_scan_get_results(wifi_scan_item_t *out, int max_count)
{
    if (_scan_state != WIFI_SCAN_DONE || out == RT_NULL)
        return 0;

    int count = _scan_count;
    if (count > max_count)
        count = max_count;

    rt_memcpy(out, _scan_buf, count * sizeof(wifi_scan_item_t));
    return count;
}

#endif /* RT_USING_WIFI */
