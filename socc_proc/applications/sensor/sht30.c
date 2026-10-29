/*
 * SHT30 Temperature & Humidity Sensor Driver
 *
 * Single-shot high-repeatability mode via I2C.
 * HAL: RT-Thread I2C device framework.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "sht30.h"
#include "tca9548a.h"

#define DBG_TAG "sht30"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* Single-shot measurement, high repeatability, clock stretching enabled */
#define SHT30_CMD_MEAS_H    0x2C
#define SHT30_CMD_MEAS_L    0x06

/* Soft reset */
#define SHT30_CMD_RESET_H   0x30
#define SHT30_CMD_RESET_L   0xA2

/* ======================== CRC-8 ======================== */

/**
 * CRC-8 for SHT30: polynomial 0x31, init 0xFF.
 */
static rt_uint8_t sht30_crc8(const rt_uint8_t *data, rt_uint8_t len)
{
    rt_uint8_t crc = 0xFF;
    rt_uint8_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x31;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ======================== Low-level I2C ======================== */

static rt_err_t sht30_send_cmd(struct rt_i2c_bus_device *bus, rt_uint8_t addr,
                                rt_uint8_t cmd_h, rt_uint8_t cmd_l)
{
    rt_uint8_t buf[2] = { cmd_h, cmd_l };
    tca9548a_device_t mux = tca9548a_shared();
    rt_err_t err;
    if (!mux || tca9548a_lock(mux) != RT_EOK) return -RT_ERROR;
    err = tca9548a_select_channel(mux, 3);
    if (err == RT_EOK && rt_i2c_master_send(bus, addr, 0, buf, 2) != 2)
        err = -RT_ERROR;
    tca9548a_unlock(mux);
    return err;
}

/* ======================== Public API ======================== */

rt_err_t sht30_reset(sht30_device_t dev)
{
    rt_err_t result;

    RT_ASSERT(dev);

    result = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK) return result;
    result = sht30_send_cmd(dev->i2c, dev->addr, SHT30_CMD_RESET_H, SHT30_CMD_RESET_L);
    rt_mutex_release(dev->lock);
    return result;
}

rt_err_t sht30_read(sht30_device_t dev, struct sht30_data *data)
{
    rt_uint8_t buf[6];
    rt_uint16_t raw_temp, raw_humi;
    rt_err_t result;

    RT_ASSERT(dev);
    RT_ASSERT(data);

    result = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
        return -RT_ERROR;

    /* Send measurement command */
    if (sht30_send_cmd(dev->i2c, dev->addr, SHT30_CMD_MEAS_H, SHT30_CMD_MEAS_L) != RT_EOK)
    {
        LOG_E("SHT30 send meas cmd failed");
        rt_mutex_release(dev->lock);
        return -RT_ERROR;
    }

    /* Wait for measurement (~15ms typical for high repeatability) */
    rt_thread_mdelay(20);

    /* 转换等待期间释放 mux；读回前重新选 CH3，避免阻塞 ECG 20ms。 */
    tca9548a_device_t mux = tca9548a_shared();
    result = mux ? tca9548a_lock(mux) : -RT_ERROR;
    if (result == RT_EOK)
    {
        result = tca9548a_select_channel(mux, 3);
        if (result == RT_EOK && rt_i2c_master_recv(dev->i2c, dev->addr, 0, buf, 6) != 6)
            result = -RT_ERROR;
        tca9548a_unlock(mux);
    }
    if (result != RT_EOK)
    {
        LOG_E("SHT30 read data failed");
        rt_mutex_release(dev->lock);
        return -RT_ERROR;
    }

    rt_mutex_release(dev->lock);

    /* Verify CRC */
    if (sht30_crc8(&buf[0], 2) != buf[2])
    {
        LOG_E("SHT30 temperature CRC mismatch");
        return -RT_ERROR;
    }
    if (sht30_crc8(&buf[3], 2) != buf[5])
    {
        LOG_E("SHT30 humidity CRC mismatch");
        return -RT_ERROR;
    }

    /* Convert raw to physical values */
    raw_temp = ((rt_uint16_t)buf[0] << 8) | buf[1];
    raw_humi = ((rt_uint16_t)buf[3] << 8) | buf[4];

    data->temperature = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    data->humidity    = 100.0f * ((float)raw_humi / 65535.0f);

    return RT_EOK;
}

sht30_device_t sht30_init_addr(const char *i2c_bus_name, rt_uint8_t addr)
{
    sht30_device_t dev;

    RT_ASSERT(i2c_bus_name);

    dev = rt_calloc(1, sizeof(struct sht30_device));
    if (dev == RT_NULL)
    {
        LOG_E("SHT30 alloc failed");
        return RT_NULL;
    }

    dev->addr = addr;

    dev->i2c = rt_i2c_bus_device_find(i2c_bus_name);
    if (dev->i2c == RT_NULL)
    {
        LOG_E("I2C bus '%s' not found", i2c_bus_name);
        rt_free(dev);
        return RT_NULL;
    }

    dev->lock = rt_mutex_create("mutex_sht30", RT_IPC_FLAG_FIFO);
    if (dev->lock == RT_NULL)
    {
        LOG_E("SHT30 mutex create failed");
        rt_free(dev);
        return RT_NULL;
    }

    /* Soft reset to ensure known state */
    if (sht30_send_cmd(dev->i2c, addr, SHT30_CMD_RESET_H, SHT30_CMD_RESET_L) != RT_EOK)
    {
        LOG_E("SHT30 soft reset failed, check wiring and addr(0x%02X)", addr);
        rt_mutex_delete(dev->lock);
        rt_free(dev);
        return RT_NULL;
    }
    rt_thread_mdelay(2);  /* reset takes ~1ms */

    LOG_I("SHT30 init OK, bus: %s, addr: 0x%02X", i2c_bus_name, addr);
    return dev;
}

void sht30_deinit(sht30_device_t dev)
{
    RT_ASSERT(dev);
    rt_mutex_delete(dev->lock);
    rt_free(dev);
}
