/*
 * TCA9548A 8 通道 I2C mux 驱动
 *
 * 单字节控制寄存器，无子地址：写入即选通道，读出即回读当前选通状态。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "tca9548a.h"

#define DBG_TAG "tca9548a"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* ======================== 低层 I2C（不加锁） ======================== */

static rt_err_t write_channel(struct rt_i2c_bus_device *bus, rt_uint8_t mask)
{
    if (rt_i2c_master_send(bus, TCA9548A_ADDR, 0, &mask, 1) == 1)
        return RT_EOK;
    else
        return -RT_ERROR;
}

static rt_err_t read_channel(struct rt_i2c_bus_device *bus, rt_uint8_t *mask)
{
    if (rt_i2c_master_recv(bus, TCA9548A_ADDR, 0, mask, 1) == 1)
        return RT_EOK;
    else
        return -RT_ERROR;
}

/* ======================== 公共 API ======================== */

rt_err_t tca9548a_set_channel(tca9548a_device_t dev, rt_uint8_t channel_mask)
{
    rt_err_t result;

    RT_ASSERT(dev);

    result = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
        return result;

    result = write_channel(dev->i2c, channel_mask);
    rt_mutex_release(dev->lock);
    return result;
}

rt_err_t tca9548a_get_channel(tca9548a_device_t dev, rt_uint8_t *channel_mask)
{
    rt_err_t result;

    RT_ASSERT(dev);
    RT_ASSERT(channel_mask);

    result = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
        return result;

    result = read_channel(dev->i2c, channel_mask);
    rt_mutex_release(dev->lock);
    return result;
}

rt_err_t tca9548a_select_channel(tca9548a_device_t dev, rt_uint8_t channel)
{
    if (channel > 7)
    {
        LOG_E("channel %d out of range (0-7)", channel);
        return -RT_ERROR;
    }
    return tca9548a_set_channel(dev, (rt_uint8_t)(1u << channel));
}

/* ======================== 进程内共享单例 ======================== */

static tca9548a_device_t s_shared_mux = RT_NULL;
static struct rt_mutex s_shared_init_lock;
static rt_bool_t s_shared_lock_ready;

tca9548a_device_t tca9548a_shared(void)
{
    /* 启动未接拓展板时，后续多个任务会并发重试；初始化也必须串行。 */
    if (!s_shared_lock_ready) return RT_NULL;
    if (rt_mutex_take(&s_shared_init_lock, RT_WAITING_FOREVER) != RT_EOK) return RT_NULL;
    if (s_shared_mux == RT_NULL)
    {
        s_shared_mux = tca9548a_init("i2c1");
    }
    rt_mutex_release(&s_shared_init_lock);
    return s_shared_mux;
}

/*
 * INIT_DEVICE_EXPORT 阶段（main 线程单线程上下文，i2c1 已在 BOARD 阶段注册）
 * 预建单例：ppg/ecg/gsr/mlx90614 四个任务线程起来后并发首调 tca9548a_shared()
 * 时单例已就绪，堵住上面 check-then-act 懒加载的 double-init 竞态窗口。
 * 拓展板未插时保留初始化锁，各任务稍后重试同一个受保护的单例。
 */
static int tca9548a_shared_prepare(void)
{
    if (rt_mutex_init(&s_shared_init_lock, "mux_init", RT_IPC_FLAG_PRIO) != RT_EOK)
        return -RT_ERROR;
    s_shared_lock_ready = RT_TRUE;
    if (tca9548a_shared() == RT_NULL)
    {
        LOG_W("TCA9548A shared mux pre-init failed (expansion board absent?)");
    }
    return 0;
}
INIT_DEVICE_EXPORT(tca9548a_shared_prepare);

rt_err_t tca9548a_lock(tca9548a_device_t dev)
{
    RT_ASSERT(dev);
    return rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
}

void tca9548a_unlock(tca9548a_device_t dev)
{
    RT_ASSERT(dev);
    rt_mutex_release(dev->lock);
}

/**
 * 初始化 TCA9548A 驱动实例。
 *
 * 步骤：alloc -> 找 I2C 总线 -> 建互斥锁 -> 写 0x00 关闭全部通道 -> 回读校验。
 */
tca9548a_device_t tca9548a_init(const char *i2c_bus_name)
{
    tca9548a_device_t dev;
    rt_uint8_t readback;

    RT_ASSERT(i2c_bus_name);

    dev = rt_calloc(1, sizeof(struct tca9548a_device));
    if (dev == RT_NULL)
    {
        LOG_E("TCA9548A alloc failed, bus: %s", i2c_bus_name);
        return RT_NULL;
    }

    dev->i2c = rt_i2c_bus_device_find(i2c_bus_name);
    if (dev->i2c == RT_NULL)
    {
        LOG_E("I2C bus '%s' not found, check menuconfig", i2c_bus_name);
        rt_free(dev);
        return RT_NULL;
    }

    dev->lock = rt_mutex_create("mutex_tca9548a", RT_IPC_FLAG_FIFO);
    if (dev->lock == RT_NULL)
    {
        LOG_E("TCA9548A mutex create failed");
        rt_free(dev);
        return RT_NULL;
    }

    if (write_channel(dev->i2c, TCA9548A_CH_NONE) != RT_EOK)
    {
        LOG_E("TCA9548A no ACK, check wiring/power on %s addr 0x%02X", i2c_bus_name, TCA9548A_ADDR);
        rt_mutex_delete(dev->lock);
        rt_free(dev);
        return RT_NULL;
    }

    if (read_channel(dev->i2c, &readback) != RT_EOK || readback != TCA9548A_CH_NONE)
    {
        LOG_E("TCA9548A readback mismatch: got 0x%02X, expected 0x%02X", readback, TCA9548A_CH_NONE);
        rt_mutex_delete(dev->lock);
        rt_free(dev);
        return RT_NULL;
    }

    LOG_I("TCA9548A init OK, bus: %s, addr: 0x%02X", i2c_bus_name, TCA9548A_ADDR);
    return dev;
}

/** 释放 TCA9548A 驱动资源 */
void tca9548a_deinit(tca9548a_device_t dev)
{
    RT_ASSERT(dev);
    rt_mutex_delete(dev->lock);
    rt_free(dev);
}
