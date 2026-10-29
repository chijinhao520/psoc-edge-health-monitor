/**
 * @file  wifi_connect.h
 * @brief WiFi 连接与扫描公共接口（供 UI 层调用）
 */
#ifndef __WIFI_CONNECT_H__
#define __WIFI_CONNECT_H__

#include <rtthread.h>

#ifdef RT_USING_WIFI

/* ====== 扫描结果 ====== */

#define WIFI_SSID_MAX_LEN  33   /* 32 + '\0' */
#define WIFI_SCAN_MAX      20   /* 最多缓存的 AP 数量 */

typedef struct {
    char    ssid[WIFI_SSID_MAX_LEN];
    int16_t rssi;           /* dBm，负值，越大信号越好 */
    uint8_t has_password;   /* 1=需要密码（WPA/WPA2/WEP） */
    uint8_t channel;
} wifi_scan_item_t;

/* 扫描状态 */
enum {
    WIFI_SCAN_IDLE      = 0,  /* 空闲，未扫描 */
    WIFI_SCAN_RUNNING   = 1,  /* 扫描中（阻塞 ~10s） */
    WIFI_SCAN_DONE      = 2,  /* 扫描完成，结果可用 */
    WIFI_SCAN_FAILED    = 3,  /* 扫描失败 */
};

/* ====== API ====== */

/** 查询 WiFi 连接状态 */
rt_bool_t wifi_is_connected(void);

/** 获取当前已连接的 SSID（未连接返回 NULL） */
const char *wifi_get_connected_ssid(void);

/** 获取当前 IP 地址字符串（未连接返回 NULL） */
const char *wifi_get_ip_str(void);

/** 启动异步扫描（创建后台线程执行，非阻塞） */
void wifi_start_scan(void);

/* 手动连接状态 */
enum {
    WIFI_CONN_IDLE       = 0,  /* 空闲 */
    WIFI_CONN_CONNECTING = 1,  /* 连接中 */
    WIFI_CONN_SUCCESS    = 2,  /* 成功 */
    WIFI_CONN_FAILED     = 3,  /* 失败 */
};

/**
 * 手动连接到指定 SSID+密码（异步，起后台线程执行，立即返回）。
 * 开放网络(无密码) password 传 "" 或 NULL 即可。
 * 用 wifi_manual_connect_status() 轮询进度。
 */
void wifi_connect_to(const char *ssid, const char *password);

/** 查询手动连接进度（WIFI_CONN_*） */
int wifi_manual_connect_status(void);

/** 查询扫描状态 */
int wifi_scan_status(void);

/**
 * 获取扫描结果（仅在 wifi_scan_status() == WIFI_SCAN_DONE 时有效）
 * @param out        输出数组
 * @param max_count  数组容量
 * @return           实际拷贝的 AP 数量
 */
int wifi_scan_get_results(wifi_scan_item_t *out, int max_count);

#endif /* RT_USING_WIFI */
#endif /* __WIFI_CONNECT_H__ */
