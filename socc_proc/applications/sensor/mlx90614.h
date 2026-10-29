/*
 * MLX90614 Infrared Temperature Sensor Driver
 *
 * I2C address: 0x5A (default, ADDR pin low)
 * Bus: i2c1 (software I2C, RPI Pin 3/5)
 * Purpose: non-contact infrared body temperature for BMR calculation
 */

#ifndef __MLX90614_H__
#define __MLX90614_H__

#include <rtthread.h>
#include <rtdevice.h>

struct mlx90614_device
{
    struct rt_i2c_bus_device *i2c;
    rt_mutex_t lock;
};
typedef struct mlx90614_device *mlx90614_device_t;

struct mlx90614_data
{
    float obj_temp;     /* object temperature, degC */
    float amb_temp;     /* ambient temperature, degC */
};

/**
 * @brief Initialize MLX90614 driver instance
 *
 * @param i2c_bus_name  RT-Thread I2C bus name (e.g. "i2c1")
 * @return device handle on success, RT_NULL on failure
 */
mlx90614_device_t mlx90614_init(const char *i2c_bus_name);
/* 调用者在共享mux锁外等待上电稳定；此入口只做一次探测，不延时300ms。 */
mlx90614_device_t mlx90614_try_init(const char *i2c_bus_name);

/**
 * @brief Free MLX90614 driver resources
 */
void mlx90614_deinit(mlx90614_device_t dev);

/**
 * @brief Read object and ambient temperature
 *
 * Uses SMBus read-word protocol with PEC (CRC-8) verification.
 *
 * @param dev   device handle
 * @param data  output: object temperature and ambient temperature (degC)
 * @return RT_EOK on success, -RT_ERROR on failure
 */
rt_err_t mlx90614_read(mlx90614_device_t dev, struct mlx90614_data *data);

/**
 * @brief Read object temperature only (convenience API)
 *
 * @param dev   device handle
 * @param temp  output: object temperature in degC
 * @return RT_EOK on success, -RT_ERROR on failure
 */
rt_err_t mlx90614_read_obj_temp(mlx90614_device_t dev, float *temp);

#endif /* __MLX90614_H__ */
