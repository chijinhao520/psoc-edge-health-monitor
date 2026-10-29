/*
 * SHT30 Temperature & Humidity Sensor Driver
 *
 * 默认 I2C address: 0x44 (ADDR→GND), 可通过 sht30_init_addr() 指定 0x45 (ADDR→VDD)
 * Bus: i2c1 (software I2C, RPI Pin 3/5)
 *
 * 角色随硬件形态切换：
 *   模块阶段 — 外接远离板子，测真实环境温湿度
 *   集成板阶段 — 贴指腹，测接触温度和局部湿度
 */

#ifndef __SHT30_H__
#define __SHT30_H__

#include <rtthread.h>
#include <rtdevice.h>

struct sht30_device
{
    struct rt_i2c_bus_device *i2c;
    rt_mutex_t lock;
    rt_uint8_t addr;    /* 7-bit I2C 地址：0x44 或 0x45 */
};
typedef struct sht30_device *sht30_device_t;

struct sht30_data
{
    float temperature;  /* degC */
    float humidity;     /* %RH  */
};

/**
 * @brief Initialize SHT30 with specified I2C address
 *
 * @param i2c_bus_name  RT-Thread I2C bus name (e.g. "i2c1")
 * @param addr          7-bit I2C address (0x44 or 0x45)
 * @return device handle on success, RT_NULL on failure
 */
sht30_device_t sht30_init_addr(const char *i2c_bus_name, rt_uint8_t addr);

/** @brief Initialize SHT30 with default address 0x44 */
static inline sht30_device_t sht30_init(const char *i2c_bus_name)
{
    return sht30_init_addr(i2c_bus_name, 0x44);
}

/**
 * @brief Free SHT30 driver resources
 */
void sht30_deinit(sht30_device_t dev);

/**
 * @brief Read temperature and humidity (single-shot, high repeatability)
 *
 * Blocks ~15ms for measurement. CRC-8 verified.
 *
 * @param dev   device handle
 * @param data  output: temperature (degC) and humidity (%RH)
 * @return RT_EOK on success, -RT_ERROR on failure
 */
rt_err_t sht30_read(sht30_device_t dev, struct sht30_data *data);

/**
 * @brief Soft-reset SHT30
 */
rt_err_t sht30_reset(sht30_device_t dev);

#endif /* __SHT30_H__ */
