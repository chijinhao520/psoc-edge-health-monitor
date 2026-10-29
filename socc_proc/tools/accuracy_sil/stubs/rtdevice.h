#ifndef ACCURACY_RTDEVICE_H
#define ACCURACY_RTDEVICE_H
#include "rtthread.h"
struct rt_i2c_bus_device { int unused; };
struct rt_i2c_msg { rt_uint16_t addr, flags, len; rt_uint8_t *buf; };
#define RT_I2C_WR 0
#define RT_I2C_RD 1
#define RT_I2C_NO_STOP 0x40
struct rt_i2c_bus_device *rt_i2c_bus_device_find(const char *);
rt_size_t rt_i2c_transfer(struct rt_i2c_bus_device *, struct rt_i2c_msg *, rt_uint32_t);
rt_size_t rt_i2c_master_send(struct rt_i2c_bus_device *, rt_uint16_t, rt_uint16_t, const rt_uint8_t *, rt_uint32_t);
rt_size_t rt_i2c_master_recv(struct rt_i2c_bus_device *, rt_uint16_t, rt_uint16_t, rt_uint8_t *, rt_uint32_t);
#endif
