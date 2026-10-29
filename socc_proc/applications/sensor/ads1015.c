/*
 * ADS1015 驱动实现
 *
 * 寄存器访问：先写 1 字节指针寄存器选中目标寄存器，再读/写 2 字节数据（MSB 在前）。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "ads1015.h"

#define DBG_TAG "ads1015"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define REG_CONVERSION  0x00
#define REG_CONFIG      0x01

#define CFG_OS_START      0x8000  /* 写1启动单次转换；读时1表示空闲/转换已完成 */
#define CFG_MUX_SINGLE0   0x4000  /* 单端 AIN0 vs GND 的 MUX 基值，通道号累加上去 */
#define CFG_PGA_2_048V    0x0400  /* ±2.048V 量程（出厂默认档），1 LSB = 1mV */
#define CFG_MODE_SINGLE   0x0100  /* 单次转换模式 */
#define CFG_DR_1600SPS    0x0080  /* 1600 SPS（出厂默认速率） */
#define CFG_COMP_DISABLE  0x0003  /* 关闭比较器功能 */

/* ======================== 低层 I2C（不加锁） ======================== */

static rt_err_t write_reg16(struct rt_i2c_bus_device *bus, rt_uint8_t reg, rt_uint16_t val)
{
    rt_uint8_t buf[3] = { reg, (rt_uint8_t)(val >> 8), (rt_uint8_t)(val & 0xFF) };

    if (rt_i2c_master_send(bus, ADS1015_ADDR, 0, buf, 3) == 3)
        return RT_EOK;
    else
        return -RT_ERROR;
}

static rt_err_t read_reg16(struct rt_i2c_bus_device *bus, rt_uint8_t reg, rt_uint16_t *val)
{
    struct rt_i2c_msg msgs[2];
    rt_uint8_t reg_addr = reg;
    rt_uint8_t buf[2];

    msgs[0].addr  = ADS1015_ADDR;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &reg_addr;
    msgs[0].len   = 1;

    msgs[1].addr  = ADS1015_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf;
    msgs[1].len   = 2;

    if (rt_i2c_transfer(bus, msgs, 2) != 2)
        return -RT_ERROR;

    *val = ((rt_uint16_t)buf[0] << 8) | buf[1];
    return RT_EOK;
}

/* ======================== 公共 API ======================== */

rt_err_t ads1015_read_single_ended(ads1015_device_t dev, rt_uint8_t channel, rt_int16_t *raw_code)
{
    rt_err_t result;
    rt_uint16_t config;
    rt_uint16_t raw16 = 0;
    int retry;

    RT_ASSERT(dev);
    RT_ASSERT(raw_code);

    if (channel > 3)
    {
        LOG_E("channel %d out of range (0-3)", channel);
        return -RT_ERROR;
    }

    result = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
        return result;

    config = CFG_OS_START | (rt_uint16_t)((0x04u + channel) << 12)
           | CFG_PGA_2_048V | CFG_MODE_SINGLE | CFG_DR_1600SPS | CFG_COMP_DISABLE;

    if (write_reg16(dev->i2c, REG_CONFIG, config) != RT_EOK)
    {
        rt_mutex_release(dev->lock);
        return -RT_ERROR;
    }

    /* 1600SPS ≈ 625us/次，轮询 OS 位确认转换完成，最多等 5ms */
    for (retry = 0; retry < 5; retry++)
    {
        rt_thread_mdelay(1);
        if (read_reg16(dev->i2c, REG_CONFIG, &raw16) == RT_EOK && (raw16 & CFG_OS_START))
            break;
    }

    /* 转换未完成时寄存器仍可能是上一通道的旧值，不能当作本次采样。 */
    if (retry == 5)
    {
        rt_mutex_release(dev->lock);
        return -RT_ERROR;
    }

    if (read_reg16(dev->i2c, REG_CONVERSION, &raw16) != RT_EOK)
    {
        rt_mutex_release(dev->lock);
        return -RT_ERROR;
    }

    rt_mutex_release(dev->lock);

    /* 12位左对齐于16位寄存器，右移4位并保留符号扩展 */
    *raw_code = (rt_int16_t)raw16 >> 4;
    return RT_EOK;
}

/**
 * 初始化 ADS1015 驱动实例。
 *
 * 步骤：alloc -> 找 I2C 总线 -> 建互斥锁 -> 读一次 CONFIG 寄存器探测 ACK。
 */
ads1015_device_t ads1015_init(const char *i2c_bus_name)
{
    ads1015_device_t dev;
    rt_uint16_t probe;

    RT_ASSERT(i2c_bus_name);

    dev = rt_calloc(1, sizeof(struct ads1015_device));
    if (dev == RT_NULL)
    {
        LOG_E("ADS1015 alloc failed, bus: %s", i2c_bus_name);
        return RT_NULL;
    }

    dev->i2c = rt_i2c_bus_device_find(i2c_bus_name);
    if (dev->i2c == RT_NULL)
    {
        LOG_E("I2C bus '%s' not found, check menuconfig", i2c_bus_name);
        rt_free(dev);
        return RT_NULL;
    }

    dev->lock = rt_mutex_create("mutex_ads1015", RT_IPC_FLAG_FIFO);
    if (dev->lock == RT_NULL)
    {
        LOG_E("ADS1015 mutex create failed");
        rt_free(dev);
        return RT_NULL;
    }

    if (read_reg16(dev->i2c, REG_CONFIG, &probe) != RT_EOK)
    {
        LOG_E("ADS1015 no ACK, check mux channel/wiring/power, addr 0x%02X", ADS1015_ADDR);
        rt_mutex_delete(dev->lock);
        rt_free(dev);
        return RT_NULL;
    }

    LOG_I("ADS1015 init OK, bus: %s, addr: 0x%02X, config: 0x%04X", i2c_bus_name, ADS1015_ADDR, probe);
    return dev;
}

/** 释放 ADS1015 驱动资源 */
void ads1015_deinit(ads1015_device_t dev)
{
    RT_ASSERT(dev);
    rt_mutex_delete(dev->lock);
    rt_free(dev);
}
