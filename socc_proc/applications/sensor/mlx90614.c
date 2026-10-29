/*
 * MLX90614 Infrared Temperature Sensor Driver
 *
 * SMBus read-word protocol via RT-Thread I2C framework.
 * PEC (CRC-8) verified for data integrity.
 *
 * Ported from GY-906 reference (BL616 GPIO bit-bang → RT-Thread I2C).
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "mlx90614.h"

#define DBG_TAG "mlx90614"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define MLX90614_ADDR       0x5A    /* 7-bit I2C address */

/* RAM register addresses */
#define MLX90614_RAM_TA     0x06    /* ambient temperature */
#define MLX90614_RAM_TOBJ1  0x07    /* object 1 temperature */

/* ======================== SMBus PEC (CRC-8) ======================== */

/**
 * SMBus CRC-8: polynomial 0x07, init 0x00.
 *
 * PEC covers the entire transaction:
 *   [slave_addr_W, command, slave_addr_R, data_low, data_high]
 */
static rt_uint8_t mlx90614_pec(const rt_uint8_t *data, rt_uint8_t len)
{
    rt_uint8_t crc = 0x00;
    rt_uint8_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ======================== Low-level I2C ======================== */

/**
 * SMBus read-word: send command byte, read 3 bytes (DataL, DataH, PEC).
 *
 * Returns raw 16-bit value on success, -1 on failure.
 */
static rt_int32_t mlx90614_read_word(struct rt_i2c_bus_device *bus,
                                      rt_uint8_t command)
{
    rt_uint8_t cmd = command;
    rt_uint8_t buf[3];  /* [DataL, DataH, PEC] */
    struct rt_i2c_msg msgs[2];
    rt_uint8_t pec_buf[5];
    rt_uint8_t pec_calc;

    /* Message 1: write command byte (NO_STOP → repeated start before read) */
    msgs[0].addr  = MLX90614_ADDR;
    msgs[0].flags = RT_I2C_WR | RT_I2C_NO_STOP;
    msgs[0].buf   = &cmd;
    msgs[0].len   = 1;

    /* Message 2: read 3 bytes (LSB, MSB, PEC) */
    msgs[1].addr  = MLX90614_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf;
    msgs[1].len   = 3;

    if (rt_i2c_transfer(bus, msgs, 2) != 2)
    {
        LOG_E("MLX90614 i2c_transfer failed (cmd=0x%02X)", command);
        return -1;
    }

    LOG_D("MLX90614 raw: cmd=0x%02X, DataL=0x%02X, DataH=0x%02X, PEC=0x%02X",
          command, buf[0], buf[1], buf[2]);

    /* Verify PEC: CRC-8 over [addr_W, cmd, addr_R, DataL, DataH] */
    pec_buf[0] = (MLX90614_ADDR << 1);         /* slave addr + write */
    pec_buf[1] = command;
    pec_buf[2] = (MLX90614_ADDR << 1) | 0x01;  /* slave addr + read */
    pec_buf[3] = buf[0];                         /* DataL */
    pec_buf[4] = buf[1];                         /* DataH */

    pec_calc = mlx90614_pec(pec_buf, 5);
    if (pec_calc != buf[2])
    {
        LOG_E("MLX90614 PEC mismatch: calc=0x%02X, recv=0x%02X (cmd=0x%02X)",
              pec_calc, buf[2], command);
        return -1;
    }

    return ((rt_uint16_t)buf[1] << 8) | buf[0];
}

/* ======================== Public API ======================== */

rt_err_t mlx90614_read(mlx90614_device_t dev, struct mlx90614_data *data)
{
    rt_int32_t raw_obj, raw_amb;
    rt_err_t result;

    RT_ASSERT(dev);
    RT_ASSERT(data);

    result = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
        return -RT_ERROR;

    raw_obj = mlx90614_read_word(dev->i2c, MLX90614_RAM_TOBJ1);
    rt_thread_mdelay(5);  /* MLX90614 SMBus 连续读需要间隔，否则第二次返回 0xFF */
    raw_amb = mlx90614_read_word(dev->i2c, MLX90614_RAM_TA);

    rt_mutex_release(dev->lock);

    if (raw_obj < 0 || raw_amb < 0)
    {
        LOG_E("MLX90614 read failed");
        return -RT_ERROR;
    }

    /* Convert: temp(degC) = raw * 0.02 - 273.15 */
    data->obj_temp = (float)raw_obj * 0.02f - 273.15f;
    data->amb_temp = (float)raw_amb * 0.02f - 273.15f;

    return RT_EOK;
}

rt_err_t mlx90614_read_obj_temp(mlx90614_device_t dev, float *temp)
{
    rt_int32_t raw;
    rt_err_t result;

    RT_ASSERT(dev);
    RT_ASSERT(temp);

    result = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
        return -RT_ERROR;

    raw = mlx90614_read_word(dev->i2c, MLX90614_RAM_TOBJ1);

    rt_mutex_release(dev->lock);

    if (raw < 0)
        return -RT_ERROR;

    *temp = (float)raw * 0.02f - 273.15f;
    return RT_EOK;
}

static mlx90614_device_t init_device(const char *i2c_bus_name, int wait_power)
{
    mlx90614_device_t dev;
    rt_int32_t raw;

    RT_ASSERT(i2c_bus_name);

    dev = rt_calloc(1, sizeof(struct mlx90614_device));
    if (dev == RT_NULL)
    {
        LOG_E("MLX90614 alloc failed");
        return RT_NULL;
    }

    dev->i2c = rt_i2c_bus_device_find(i2c_bus_name);
    if (dev->i2c == RT_NULL)
    {
        LOG_E("I2C bus '%s' not found", i2c_bus_name);
        rt_free(dev);
        return RT_NULL;
    }

    dev->lock = rt_mutex_create("mutex_mlx", RT_IPC_FLAG_FIFO);
    if (dev->lock == RT_NULL)
    {
        LOG_E("MLX90614 mutex create failed");
        rt_free(dev);
        return RT_NULL;
    }

    /* MLX90614 needs ~250ms after power-up to output valid data */
    if (wait_power) rt_thread_mdelay(300);

    /* Probe: try reading object temperature to verify communication */
    raw = mlx90614_read_word(dev->i2c, MLX90614_RAM_TOBJ1);
    if (raw < 0)
    {
        LOG_E("MLX90614 probe failed, check wiring and addr(0x5A)");
        rt_mutex_delete(dev->lock);
        rt_free(dev);
        return RT_NULL;
    }

    LOG_I("MLX90614 init OK, bus: %s, addr: 0x%02X, probe temp: %.2f C",
          i2c_bus_name, MLX90614_ADDR, (float)raw * 0.02f - 273.15f);
    return dev;
}

mlx90614_device_t mlx90614_init(const char *name) { return init_device(name,1); }
mlx90614_device_t mlx90614_try_init(const char *name) { return init_device(name,0); }

void mlx90614_deinit(mlx90614_device_t dev)
{
    RT_ASSERT(dev);
    rt_mutex_delete(dev->lock);
    rt_free(dev);
}
