/*
 * ADS1015 4通道 12-bit I2C ADC 驱动头文件
 *
 * 硬件连接（拓展板 v4.3.1，挂在 TCA9548A mux CH4 下游）：
 *   AIN0 ← AD8232 ECG OUTPUT
 *   AIN1 ← Grove GSR
 *   AIN2/AIN3 预留
 *   ADDR → GND，I2C 地址固定 0x48
 *
 * 使用前必须先经 tca9548a_select_channel(mux_dev, 4) 选通 CH4，
 * 否则这颗芯片在 i2c1 总线上不可见。
 */

#ifndef __ADS1015_H__
#define __ADS1015_H__

#include <rtthread.h>
#include <rtdevice.h>

#define ADS1015_ADDR  0x48   /* 7位地址，ADDR 接 GND */

/* ======================== 设备结构体 ======================== */

struct ads1015_device
{
    struct rt_i2c_bus_device *i2c;  /* 绑定的 RT-Thread I2C 总线设备 */
    rt_mutex_t lock;                /* 互斥锁，保护 I2C 并发访问 */
};
typedef struct ads1015_device *ads1015_device_t;

/* ======================== 公共 API ======================== */

/**
 * @brief 初始化 ADS1015 驱动实例
 *
 * @param i2c_bus_name RT-Thread I2C 总线名称（本项目传 "i2c1"，调用前需已选通 mux CH4）
 * @return 成功返回设备句柄，失败返回 RT_NULL
 */
ads1015_device_t ads1015_init(const char *i2c_bus_name);

/** @brief 释放 ADS1015 驱动资源 */
void ads1015_deinit(ads1015_device_t dev);

/**
 * @brief 单端采样一个通道（AIN_channel vs GND），触发单次转换并阻塞等待完成
 *
 * 量程固定 ±2.048V（PGA 默认档），此量程下 1 LSB = 1mV。
 *
 * @param dev      设备句柄
 * @param channel  通道号 0-3（对应 AIN0-AIN3）
 * @param raw_code 输出：12位有符号原始码（-2048~2047），数值即约等于 mV
 * @return RT_EOK 成功，-RT_ERROR 失败或 channel 超出范围
 */
rt_err_t ads1015_read_single_ended(ads1015_device_t dev, rt_uint8_t channel, rt_int16_t *raw_code);

#endif /* __ADS1015_H__ */
