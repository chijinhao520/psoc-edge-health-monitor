/*
 * Derived from Maxim MAXREFDES117 reference code.
 * Original notice: LICENSES/Maxim-Integrated.txt
 * Project modifications: RT-Thread port, timing and validity fixes.
 */
/*
 * MAX30102 Heart-Rate / SpO2 Sensor Driver
 *
 * Ported from Maxim MAXREFDES117 (mbed reference design).
 * HAL replaced with RT-Thread I2C device framework.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "MAX30102.h"

#define DBG_TAG "max30102"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define MAX30102_ADDR  0x57   /* 7-bit I2C address (ADDR pin tied low) */

/* ======================== Low-level I2C (no lock) ======================== */

static rt_err_t write_reg(struct rt_i2c_bus_device *bus,
                           rt_uint8_t uch_addr,
                           rt_uint8_t uch_data)
{
    rt_uint8_t buf[2] = { uch_addr, uch_data };

    if (rt_i2c_master_send(bus, MAX30102_ADDR, 0, buf, 2) == 2)
        return RT_EOK;
    else
        return -RT_ERROR;
}

static rt_err_t read_reg(struct rt_i2c_bus_device *bus,
                          rt_uint8_t uch_addr,
                          rt_uint8_t *puch_data)
{
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = MAX30102_ADDR;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &uch_addr;
    msgs[0].len   = 1;

    msgs[1].addr  = MAX30102_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = puch_data;
    msgs[1].len   = 1;

    if (rt_i2c_transfer(bus, msgs, 2) == 2)
        return RT_EOK;
    else
        return -RT_ERROR;
}

/* ======================== Internal Config ======================== */

/* 无平均、100SPS、18 位；禁用 rollover，溢出后整个窗口作废。 */
static rt_err_t sensor_config(struct rt_i2c_bus_device *bus)
{
    if (write_reg(bus, REG_FIFO_WR_PTR,   0x00) != RT_EOK) return -RT_ERROR;
    if (write_reg(bus, REG_OVF_COUNTER,   0x00) != RT_EOK) return -RT_ERROR;
    if (write_reg(bus, REG_FIFO_RD_PTR,   0x00) != RT_EOK) return -RT_ERROR;

    if (write_reg(bus, REG_INTR_ENABLE_1, 0xC0) != RT_EOK) return -RT_ERROR;
    if (write_reg(bus, REG_INTR_ENABLE_2, 0x00) != RT_EOK) return -RT_ERROR;
    if (write_reg(bus, REG_FIFO_CONFIG,   0x0F) != RT_EOK) return -RT_ERROR;
    if (write_reg(bus, REG_SPO2_CONFIG,   0x27) != RT_EOK) return -RT_ERROR;

    if (write_reg(bus, REG_LED1_PA,       0x24) != RT_EOK) return -RT_ERROR;
    if (write_reg(bus, REG_LED2_PA,       0x24) != RT_EOK) return -RT_ERROR;
    if (write_reg(bus, REG_PILOT_PA,      0x7F) != RT_EOK) return -RT_ERROR;
    if (write_reg(bus, REG_MODE_CONFIG,   0x03) != RT_EOK) return -RT_ERROR;

    return RT_EOK;
}

/* ======================== Public API ======================== */

/**
 * Soft-reset MAX30102.
 * MODE_CONFIG[6]=1 triggers reset; chip auto-clears in ~1ms.
 */
rt_err_t max30102_reset(max30102_device_t dev)
{
    rt_err_t result;

    RT_ASSERT(dev);

    result = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (result != RT_EOK) return result;
    result = write_reg(dev->i2c, REG_MODE_CONFIG, 0x40);
    rt_mutex_release(dev->lock);
    return result;
}

/* 连续寄存器和 FIFO 数据均使用单次组合事务，减少共享总线开销。 */
static rt_err_t read_bytes(struct rt_i2c_bus_device *bus, rt_uint8_t reg,
                           rt_uint8_t *data, rt_uint16_t len)
{
    struct rt_i2c_msg msgs[2];
    msgs[0].addr = MAX30102_ADDR; msgs[0].flags = RT_I2C_WR;
    msgs[0].buf = &reg; msgs[0].len = 1;
    msgs[1].addr = MAX30102_ADDR; msgs[1].flags = RT_I2C_RD;
    msgs[1].buf = data; msgs[1].len = len;
    return rt_i2c_transfer(bus, msgs, 2) == 2 ? RT_EOK : -RT_ERROR;
}

rt_err_t max30102_flush_fifo(max30102_device_t dev)
{
    rt_err_t err;
    if (!dev) return -RT_EINVAL;
    err = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (err != RT_EOK) return err;
    err = write_reg(dev->i2c, REG_FIFO_WR_PTR, 0);
    if (err == RT_EOK) err = write_reg(dev->i2c, REG_FIFO_RD_PTR, 0);
    if (err == RT_EOK) err = write_reg(dev->i2c, REG_OVF_COUNTER, 0);
    rt_mutex_release(dev->lock);
    return err;
}

rt_err_t max30102_read_status(max30102_device_t dev, rt_uint8_t *wr,
                              rt_uint8_t *rd, rt_uint8_t *ovf)
{
    rt_uint8_t status[3];
    rt_err_t err;
    if (!dev) return -RT_EINVAL;
    err = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (err != RT_EOK) return err;
    err = read_bytes(dev->i2c, REG_FIFO_WR_PTR, status, 3);
    if (err == RT_EOK)
    {
        if (wr) *wr = status[0] & 31;
        if (ovf) *ovf = status[1] & 31;
        if (rd) *rd = status[2] & 31;
    }
    rt_mutex_release(dev->lock);
    return err;
}

rt_err_t max30102_read_batch(max30102_device_t dev, rt_uint32_t *red,
                             rt_uint32_t *ir, rt_uint16_t capacity,
                             rt_uint16_t *count)
{
    rt_uint8_t status[3], raw[MAX30102_BATCH_MAX * 6];
    rt_uint16_t n, i;
    rt_err_t err;
    if (count) *count = 0;
    if (!dev || !red || !ir || !count || !capacity) return -RT_EINVAL;
    err = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (err != RT_EOK) return err;
    err = read_bytes(dev->i2c, REG_FIFO_WR_PTR, status, 3);
    if (err == RT_EOK)
    {
        n = (status[0] - status[2]) & 31;
        /* 高水位也拒绝，避免读期间变满而被读操作清掉 OVF 证据。 */
        if ((status[1] & 31) || n >= 24) err = -RT_EFULL;
        else if (n)
        {
            if (n > capacity) n = capacity;
            if (n > MAX30102_BATCH_MAX) n = MAX30102_BATCH_MAX;
            err = read_bytes(dev->i2c, REG_FIFO_DATA, raw, n * 6);
            if (err == RT_EOK)
            {
                for (i = 0; i < n; i++)
                {
                    red[i] = (((rt_uint32_t)raw[i*6] << 16) |
                              ((rt_uint32_t)raw[i*6+1] << 8) | raw[i*6+2]) & 0x3FFFF;
                    ir[i] = (((rt_uint32_t)raw[i*6+3] << 16) |
                             ((rt_uint32_t)raw[i*6+4] << 8) | raw[i*6+5]) & 0x3FFFF;
                }
                *count = n;
            }
        }
    }
    rt_mutex_release(dev->lock);
    return err;
}

rt_err_t max30102_read_fifo(max30102_device_t dev, rt_uint32_t *red, rt_uint32_t *ir)
{
    rt_uint16_t n;
    rt_err_t err = max30102_read_batch(dev, red, ir, 1, &n);
    return (err == RT_EOK && !n) ? -RT_EEMPTY : err;
}

/**
 * Initialize MAX30102 driver instance.
 *
 * Steps: alloc -> find I2C bus -> create mutex -> soft reset -> configure.
 *
 * @param i2c_bus_name  RT-Thread I2C bus name (e.g. "i2c1")
 * @return device handle on success, RT_NULL on failure
 */
static max30102_device_t init_device(const char *i2c_bus_name, int attempts)
{
    max30102_device_t dev;

    RT_ASSERT(i2c_bus_name);

    dev = rt_calloc(1, sizeof(struct max30102_device));
    if (dev == RT_NULL)
    {
        LOG_E("MAX30102 alloc failed, bus: %s", i2c_bus_name);
        return RT_NULL;
    }

    dev->i2c = rt_i2c_bus_device_find(i2c_bus_name);
    if (dev->i2c == RT_NULL)
    {
        LOG_E("I2C bus '%s' not found, check menuconfig", i2c_bus_name);
        rt_free(dev);
        return RT_NULL;
    }

    dev->lock = rt_mutex_create("mutex_max30102", RT_IPC_FLAG_FIFO);
    if (dev->lock == RT_NULL)
    {
        LOG_E("MAX30102 mutex create failed");
        rt_free(dev);
        return RT_NULL;
    }

    /*
     * Probe MAX30102 via PART_ID (0xFF, expected 0x15) with retries.
     * Background: INIT_APP_EXPORT runs while LCD/NPU/WiFi are also coming up,
     * causing 3.3V dip and bus glitches; meanwhile MAX30102 PoR (~1.5ms in
     * datasheet) is extended by flying-wire VCC ramp. Single-shot probe at
     * t≈25s NACKs, but i2c_scan at t≈30s succeeds.
     * Retry up to 10 × 50ms (≤500ms total) — much shorter than the failure
     * we saw, but enough to ride out the worst of the boot power transient.
     */
    if (attempts > 1) rt_thread_mdelay(50);
    {
        rt_uint8_t part_id = 0;
        rt_err_t   probe_err = -RT_ERROR;
        int        attempt;

        for (attempt = 1; attempt <= attempts; attempt++)
        {
            probe_err = read_reg(dev->i2c, REG_PART_ID, &part_id);
            if (probe_err == RT_EOK && part_id == 0x15)
            {
                LOG_I("MAX30102 probe OK on attempt %d, PART_ID=0x15", attempt);
                break;
            }
            if (attempt < attempts) rt_thread_mdelay(50);
        }

        if (probe_err != RT_EOK)
        {
            LOG_W("MAX30102 no ACK after %d attempt(s)", attempts);
            rt_mutex_delete(dev->lock);
            rt_free(dev);
            return RT_NULL;
        }
        if (part_id != 0x15)
        {
            LOG_E("MAX30102 PART_ID mismatch: got 0x%02X, expected 0x15", part_id);
            rt_mutex_delete(dev->lock);
            rt_free(dev);
            return RT_NULL;
        }
    }

    if (write_reg(dev->i2c, REG_MODE_CONFIG, 0x40) != RT_EOK)
    {
        LOG_E("MAX30102 soft reset failed, check I2C wiring and addr(0x57)");
        rt_mutex_delete(dev->lock);
        rt_free(dev);
        return RT_NULL;
    }
    rt_thread_mdelay(10);  /* reset takes ~1ms, use 10ms margin */

    if (sensor_config(dev->i2c) != RT_EOK)
    {
        LOG_E("MAX30102 register config failed");
        rt_mutex_delete(dev->lock);
        rt_free(dev);
        return RT_NULL;
    }

    LOG_I("MAX30102 init OK, bus: %s, addr: 0x%02X", i2c_bus_name, MAX30102_ADDR);
    return dev;
}

max30102_device_t max30102_init(const char *name) { return init_device(name, 10); }
max30102_device_t max30102_try_init(const char *name) { return init_device(name, 1); }

rt_err_t max30102_check_config(max30102_device_t dev)
{
    static const rt_uint8_t regs[] = {REG_FIFO_CONFIG, REG_MODE_CONFIG, REG_SPO2_CONFIG, REG_LED1_PA, REG_LED2_PA};
    static const rt_uint8_t expected[] = {0x0F, 0x03, 0x27, 0x24, 0x24};
    if (!dev) return -RT_EINVAL;
    rt_err_t err = rt_mutex_take(dev->lock, RT_WAITING_FOREVER);
    if (err != RT_EOK) return err;
    for (unsigned i=0; i<sizeof(regs); i++) {
        rt_uint8_t value=0;
        err=read_reg(dev->i2c, regs[i], &value);
        if (err != RT_EOK || value != expected[i]) { err=-RT_ERROR; break; }
    }
    rt_mutex_release(dev->lock);
    return err;
}

/** Free MAX30102 driver resources. */
void max30102_deinit(max30102_device_t dev)
{
    RT_ASSERT(dev);
    rt_mutex_delete(dev->lock);
    rt_free(dev);
}
