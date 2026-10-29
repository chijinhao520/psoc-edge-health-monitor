/*
 * Derived from Maxim MAXREFDES117 reference code.
 * Original notice: LICENSES/Maxim-Integrated.txt
 * Project modifications: RT-Thread port, timing and validity fixes.
 */
/*
 * MAX30102 心率/血氧传感器驱动头文件
 *
 * 移植自 Maxim MAXREFDES117 参考设计（mbed 版本）
 * 底层 HAL 替换为 RT-Thread I2C 设备框架
 *
 * 硬件连接（Edgi Talk / PSoC Edge E84）：
 *   SCL → P9.2（i2c1，经 TCA9548A CH0/CH1）
 *   SDA → P9.3（i2c1）
 *   INT → 可选，轮询模式下可不接
 *   I2C 地址：0x57（7位）
 */

#ifndef __MAX30102_H__
#define __MAX30102_H__

#include <rtthread.h>
#include <rtdevice.h>

/* ======================== 寄存器地址 ======================== */

#define REG_INTR_STATUS_1   0x00  /* 中断状态寄存器 1 */
#define REG_INTR_STATUS_2   0x01  /* 中断状态寄存器 2 */
#define REG_INTR_ENABLE_1   0x02  /* 中断使能寄存器 1 */
#define REG_INTR_ENABLE_2   0x03  /* 中断使能寄存器 2 */
#define REG_FIFO_WR_PTR     0x04  /* FIFO 写指针 */
#define REG_OVF_COUNTER     0x05  /* FIFO 溢出计数器 */
#define REG_FIFO_RD_PTR     0x06  /* FIFO 读指针 */
#define REG_FIFO_DATA       0x07  /* FIFO 数据寄存器 */
#define REG_FIFO_CONFIG     0x08  /* FIFO 配置寄存器 */
#define REG_MODE_CONFIG     0x09  /* 工作模式配置 */
#define REG_SPO2_CONFIG     0x0A  /* SpO2 配置（采样率/脉宽/ADC量程） */
#define REG_LED1_PA         0x0C  /* LED1（红光）驱动电流 */
#define REG_LED2_PA         0x0D  /* LED2（红外）驱动电流 */
#define REG_PILOT_PA        0x10  /* Pilot LED 驱动电流 */
#define REG_MULTI_LED_CTRL1 0x11  /* 多 LED 控制寄存器 1 */
#define REG_MULTI_LED_CTRL2 0x12  /* 多 LED 控制寄存器 2 */
#define REG_TEMP_INTR       0x1F  /* 芯片内部温度整数部分 */
#define REG_TEMP_FRAC       0x20  /* 芯片内部温度小数部分 */
#define REG_TEMP_CONFIG     0x21  /* 温度测量触发配置 */
#define REG_PROX_INT_THRESH 0x30  /* 接近中断阈值 */
#define REG_REV_ID          0xFE  /* 修订版本 ID */
#define REG_PART_ID         0xFF  /* 器件 ID（MAX30102 固定为 0x15） */

/* ======================== 设备结构体 ======================== */

struct max30102_device
{
    struct rt_i2c_bus_device *i2c;  /* 绑定的 RT-Thread I2C 总线设备 */
    rt_mutex_t lock;                /* 互斥锁，保护 I2C 并发访问 */
};
typedef struct max30102_device *max30102_device_t;

/* ======================== 公共 API ======================== */

/**
 * @brief 初始化 MAX30102 驱动实例
 *
 * 执行软复位 → 配置 SpO2 采样参数（100Hz，400µs 脉宽，4096nA ADC 量程）
 *
 * @param i2c_bus_name RT-Thread I2C 总线名称（本项目固定传 "i2c1"）
 * @return 成功返回设备句柄，失败返回 RT_NULL
 */
max30102_device_t max30102_init(const char *i2c_bus_name);
/* 运行中恢复：单次探测，缺席时不在共享总线上长时间等待。 */
max30102_device_t max30102_try_init(const char *i2c_bus_name);
rt_err_t max30102_check_config(max30102_device_t dev);

/**
 * @brief 释放 MAX30102 驱动资源
 *
 * @param dev 设备句柄
 */
void max30102_deinit(max30102_device_t dev);

/* 调用者持有共享 mux 锁并检查选通结果。空 FIFO 成功返回 count=0。
 * -RT_EFULL 或通信失败必须清 FIFO 并丢弃整段；不得把读取耗时当采样周期。 */
#define MAX30102_BATCH_MAX 8
rt_err_t max30102_read_batch(max30102_device_t dev, rt_uint32_t *red,
                             rt_uint32_t *ir, rt_uint16_t capacity, rt_uint16_t *count);
rt_err_t max30102_flush_fifo(max30102_device_t dev);
rt_err_t max30102_read_fifo(max30102_device_t dev, rt_uint32_t *red, rt_uint32_t *ir);
rt_err_t max30102_read_status(max30102_device_t dev, rt_uint8_t *wr,
                              rt_uint8_t *rd, rt_uint8_t *ovf);

/**
 * @brief 软复位 MAX30102
 *
 * 写 MODE_CONFIG[6]=1 触发复位，约 1ms 后自动清零。
 *
 * @param dev 设备句柄
 * @return RT_EOK 成功，-RT_ERROR 失败
 */
rt_err_t max30102_reset(max30102_device_t dev);

#endif /* __MAX30102_H__ */
