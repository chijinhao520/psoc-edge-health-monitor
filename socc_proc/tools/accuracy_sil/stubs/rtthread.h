#ifndef ACCURACY_RTTHREAD_H
#define ACCURACY_RTTHREAD_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef int16_t rt_int16_t;
typedef uint32_t rt_uint32_t;
typedef uint32_t rt_tick_t;
typedef int rt_bool_t;
#define RT_TRUE 1
#define RT_FALSE 0
#define rt_memset memset
rt_tick_t rt_tick_get(void);
rt_tick_t rt_tick_from_millisecond(int);
typedef int rt_err_t;
typedef int rt_int32_t;
typedef size_t rt_size_t;
typedef void *rt_mutex_t;
#define RT_NULL NULL
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EINVAL 2
#define RT_EFULL 3
#define RT_EEMPTY 4
#define RT_WAITING_FOREVER -1
#define RT_IPC_FLAG_FIFO 0
#define RT_ASSERT assert
#define rt_calloc calloc
#define rt_free free
rt_mutex_t rt_mutex_create(const char *, int);
rt_err_t rt_mutex_take(rt_mutex_t, int);
rt_err_t rt_mutex_release(rt_mutex_t);
rt_err_t rt_mutex_delete(rt_mutex_t);
void rt_thread_mdelay(int);
#endif
