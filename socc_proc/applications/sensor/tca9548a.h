/*
 * TCA9548A 8 通道 I2C mux 驱动头文件
 *
 * 硬件连接（拓展板 v4.3.1，经 PMOD1 板对板接入）：
 *   主侧 SCL/SDA → PSoC P9.3/P9.2（与 i2c1 同一物理引脚，经 TXS0108 电平转换）
 *   A0/A1/A2 → 全接 GND，I2C 地址固定 0x70
 *
 * 协议：TCA9548A 只有一个"控制寄存器"，无子地址——
 *   写 1 字节 = 设置通道使能位图（bit i = 通道 i 是否导通，可多路同时选通）
 *   读 1 字节 = 回读当前使能位图
 */

#ifndef __TCA9548A_H__
#define __TCA9548A_H__

#include <rtthread.h>
#include <rtdevice.h>

#define TCA9548A_ADDR       0x70   /* 7位地址，A0/A1/A2 全接 GND */
#define TCA9548A_CH_NONE    0x00   /* 全部通道关闭 */

/* ======================== 设备结构体 ======================== */

struct tca9548a_device
{
    struct rt_i2c_bus_device *i2c;  /* 绑定的 RT-Thread I2C 总线设备 */
    rt_mutex_t lock;                /* 互斥锁，保护 I2C 并发访问 */
};
typedef struct tca9548a_device *tca9548a_device_t;

/* ======================== 公共 API ======================== */

/**
 * @brief 初始化 TCA9548A 驱动实例
 *
 * 初始化后将所有通道置为关闭（0x00）并回读校验，确认寄存器级通信正常。
 *
 * @param i2c_bus_name RT-Thread I2C 总线名称（本项目传 "i2c1"）
 * @return 成功返回设备句柄，失败返回 RT_NULL
 */
tca9548a_device_t tca9548a_init(const char *i2c_bus_name);

/** @brief 释放 TCA9548A 驱动资源 */
void tca9548a_deinit(tca9548a_device_t dev);

/**
 * @brief 设置通道使能位图（可多路同时选通）
 *
 * @param dev          设备句柄
 * @param channel_mask bit i = 通道 i 使能，0x00 = 全部关闭
 * @return RT_EOK 成功，-RT_ERROR 失败
 */
rt_err_t tca9548a_set_channel(tca9548a_device_t dev, rt_uint8_t channel_mask);

/**
 * @brief 回读当前通道使能位图
 *
 * @param dev          设备句柄
 * @param channel_mask 输出：当前使能位图
 * @return RT_EOK 成功，-RT_ERROR 失败
 */
rt_err_t tca9548a_get_channel(tca9548a_device_t dev, rt_uint8_t *channel_mask);

/**
 * @brief 独占选通单一通道（0-7），其余通道自动关闭
 *
 * @param dev     设备句柄
 * @param channel 通道号 0-7
 * @return RT_EOK 成功，-RT_ERROR 失败或 channel 超出范围
 */
rt_err_t tca9548a_select_channel(tca9548a_device_t dev, rt_uint8_t channel);

/**
 * @brief 获取进程内共享的 TCA9548A 单例句柄（懒加载，只在第一次调用时初始化）
 *
 * mux 是被多个使用者共用的硬件资源（ppg_task 持续轮询 CH0/CH1 采 PPG，各种
 * MSH 自测命令临时切到别的通道调试）。各自独立 tca9548a_init() 会各建一把
 * 互不相干的锁，并发访问时会互相踩通道选择——必须都从这个单例拿句柄，
 * 配合 tca9548a_lock()/tca9548a_unlock() 才有真正互斥。
 *
 * @return 成功返回共享句柄，失败返回 RT_NULL
 */
tca9548a_device_t tca9548a_shared(void);

/**
 * @brief 对共享 mux 加锁，用于包住"选通道 + 访问下游设备"这段临界区
 *
 * 和 tca9548a_select_channel 内部用的是同一把锁（RT-Thread 互斥锁支持同一
 * 线程嵌套获取，不会死锁），必须和 tca9548a_unlock() 成对调用。
 */
rt_err_t tca9548a_lock(tca9548a_device_t dev);

/** @brief 解锁，与 tca9548a_lock() 成对调用 */
void tca9548a_unlock(tca9548a_device_t dev);

#endif /* __TCA9548A_H__ */
