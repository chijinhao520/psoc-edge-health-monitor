/*
 * touch_task.c — 触摸 IPC 采集任务
 *
 * M33 通过 i2c2 (P13.1/P13.2) 以 30ms 周期轮询 ST7102（I2C 0x55）触摸坐标，
 * 写入共享内存 touch 字段。M55 lv_port_indev.c 读共享内存获取触摸事件。
 *
 * 引脚说明：
 *   ST7102 触摸屏 I2C 接在 P13.1(SCL)/P13.2(SDA)，对应 M33 的 "i2c2" 总线。
 *   这与传感器用的 i2c1 (P9.2/P9.3) 是不同的物理总线，互不干扰。
 *   RST=P17.3, IRQ=P17.2 由 M55 lv_port_indev_init() 初始化。
 *
 * ST7102 读数据格式（寄存器 0x0010，读 16 字节）：
 *   buf[0] = 0x08  → 有效触摸帧标志
 *   buf[9]  > 0    → Point 0 触摸强度（>0 表示有接触）
 *   buf[4:5]       → Point 0 X 坐标（高字节 & 0x3F，低字节）
 *   buf[6:7]       → Point 0 Y 坐标（高字节 & 0x3F，低字节）
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "ipc_shared_data.h"

#define DBG_TAG "touch_task"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* ------------------------------------------------------------------ */
/*  ST7102 硬件常量                                                     */
/* ------------------------------------------------------------------ */
#define ST7102_I2C_BUS        "i2c2"   /* P13.1(SCL)/P13.2(SDA)，触摸专用 */
#define ST7102_ADDR           0x55

#define TOUCH_TASK_PERIOD_MS  30   /* ~33Hz */

/* ------------------------------------------------------------------ */
/*  内部 I2C 句柄                                                       */
/* ------------------------------------------------------------------ */
static struct rt_i2c_bus_device *s_st7102_i2c = RT_NULL;

/* ------------------------------------------------------------------ */
/*  ST7102 最小化实现（读 16 位寄存器地址）                             */
/* ------------------------------------------------------------------ */

static rt_err_t st7102_read(rt_uint8_t reg_h, rt_uint8_t reg_l,
                             rt_uint8_t *buf, rt_size_t len)
{
    rt_uint8_t cmd[2] = { reg_h, reg_l };
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = ST7102_ADDR;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = cmd;
    msgs[0].len   = 2;

    msgs[1].addr  = ST7102_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf;
    msgs[1].len   = len;

    rt_ssize_t ret = rt_i2c_transfer(s_st7102_i2c, msgs, 2);
    if (ret != 2)
    {
        static rt_uint32_t fail_cnt = 0;
        if (fail_cnt++ % 100 == 0)
            LOG_W("i2c xfer ret=%d, fail_cnt=%u", (int)ret, fail_cnt);
        return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_err_t st7102_init(void)
{
    s_st7102_i2c = rt_i2c_bus_device_find(ST7102_I2C_BUS);
    if (s_st7102_i2c == RT_NULL)
    {
        LOG_E("Can't find i2c bus %s", ST7102_I2C_BUS);
        return -RT_ERROR;
    }
    return RT_EOK;
}

/* 与原版 drv_touch.c 一致的读取缓冲和去重机制 */
#define ST7102_READ_BUF_MAX   80
#define ST7102_READ_FALLBACK  16

static rt_uint8_t s_read_buf[ST7102_READ_BUF_MAX];
static rt_uint8_t s_last_buf[ST7102_READ_BUF_MAX];
static rt_size_t  s_read_len = ST7102_READ_BUF_MAX;

/*
 * 获取单点触摸坐标；返回 RT_EOK 表示有**新的**触摸
 *
 * ST7102 的特性：松手后寄存器仍保留最后一次触摸数据（不会自动清零）。
 * 原版 drv_touch.c 通过 Last_read_buf 对比来去重——如果当前帧和上一帧
 * 的坐标+强度完全相同，视为"无新触摸"（松手或静止）。
 */
static rt_err_t st7102_get_touch(rt_int16_t *x, rt_int16_t *y)
{
    if (st7102_read(0x00, 0x10, s_read_buf, s_read_len) != RT_EOK)
    {
        if (s_read_len > ST7102_READ_FALLBACK)
        {
            if (st7102_read(0x00, 0x10, s_read_buf, ST7102_READ_FALLBACK) == RT_EOK)
                s_read_len = ST7102_READ_FALLBACK;
            else
                return -RT_ERROR;
        }
        else
        {
            return -RT_ERROR;
        }
    }

    /* buf[0]==0x08: 有效帧; buf[9]>0: Point 0 有触摸强度 */
    if (s_read_buf[0] != 0x08 || s_read_buf[9] == 0)
    {
        rt_memcpy(s_last_buf, s_read_buf, s_read_len);
        return -RT_ERROR;
    }

    int16_t cur_x  = (s_read_buf[4] & 0x3F) << 8 | s_read_buf[5];
    int16_t cur_y  = (s_read_buf[6] & 0x3F) << 8 | s_read_buf[7];
    int16_t cur_w  = s_read_buf[9];
    int16_t last_x = (s_last_buf[4] & 0x3F) << 8 | s_last_buf[5];
    int16_t last_y = (s_last_buf[6] & 0x3F) << 8 | s_last_buf[7];
    int16_t last_w = s_last_buf[9];

    rt_memcpy(s_last_buf, s_read_buf, s_read_len);

    /* 与原版一致：坐标+强度完全相同 → 视为松手/静止，不上报 */
    if (cur_x == last_x && cur_y == last_y && cur_w == last_w)
        return -RT_ERROR;

    *x = cur_x;
    *y = cur_y;
    return RT_EOK;
}

/* ------------------------------------------------------------------ */
/*  任务主体                                                            */
/* ------------------------------------------------------------------ */

static void touch_thread_entry(void *param)
{
    rt_int16_t x = 0, y = 0;

    RT_UNUSED(param);

    /* 等待 M55 完成 ST7102 GPIO 复位（在 lv_port_indev_init() 中执行）
     * M55 由 INIT_BOARD_EXPORT 启动，RST 序列在 LCD 初始化后运行，约需 1-2s。
     * 保守等待 3s，确保 ST7102 已就绪再开始 I2C 通信。 */
    rt_thread_mdelay(3000);

    if (st7102_init() != RT_EOK)
    {
        LOG_E("ST7102 init failed, thread exit");
        return;
    }
    LOG_I("ST7102 init on %s, start polling", ST7102_I2C_BUS);

    while (1)
    {
        volatile sensor_ipc_data_t *ipc = ipc_get_data_ptr();

        if (st7102_get_touch(&x, &y) == RT_EOK)
        {
            ipc->touch_x       = x;
            ipc->touch_y       = y;
            ipc->touch_pressed = 1;
        }
        else
        {
            ipc->touch_pressed = 0;
        }
        ipc->touch_seq++;
        __asm__ volatile("dsb sy" ::: "memory");

        rt_thread_mdelay(TOUCH_TASK_PERIOD_MS);
    }
}

/* ------------------------------------------------------------------ */
/*  任务注册                                                            */
/* ------------------------------------------------------------------ */

int touch_task_init(void)
{
    rt_thread_t tid = rt_thread_create(
        "touch",
        touch_thread_entry,
        RT_NULL,
        2048,
        16,   /* ppg(12) > sht30(13) > mlx(14) > bmr(15) > touch(16) */
        10);

    if (tid == RT_NULL)
    {
        LOG_E("touch thread create failed");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(touch_task_init);
