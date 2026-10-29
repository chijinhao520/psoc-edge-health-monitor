#ifndef IPC_SNAPSHOT_H
#define IPC_SNAPSHOT_H
#include <board.h>
#include <string.h>
#include "ipc_shared_data.h"

/* Cache invalidation is repeated after the copy: the writer may have committed
 * while the reader still held an old sequence cache line. Never spin indefinitely. */
static inline void ipc_invalidate(uint32_t address, uint32_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((uint32_t *)address, size);
#else
    (void)address; (void)size;
#endif
    __asm__ volatile("dsb sy" ::: "memory");
}
static inline int ipc_sensor_snapshot(sensor_ipc_data_t *out)
{
    volatile sensor_ipc_data_t *p=ipc_get_data_ptr();
    for (int n=0;n<3;n++) {
        ipc_invalidate(IPC_USER_DATA_ADDR,sizeof(*out));
        uint32_t seq=p->sequence;
        if (seq&1) continue;
        memcpy(out,(const void *)p,sizeof(*out));
        ipc_invalidate(IPC_USER_DATA_ADDR,sizeof(*out));
        if (seq==p->sequence && out->sequence==seq && out->magic==IPC_MAGIC && p->magic==IPC_MAGIC)
            return 1;
    }
    return 0;
}
static inline int ipc_wave_snapshot(ppg_waveform_ipc_t *out)
{
    volatile ppg_waveform_ipc_t *p=ipc_get_waveform_ptr();
    for (int n=0;n<3;n++) {
        ipc_invalidate(IPC_PPG_WAVEFORM_ADDR,sizeof(*out));
        uint32_t seq=p->sequence;
        if (seq&1) continue;
        memcpy(out,(const void *)p,sizeof(*out));
        ipc_invalidate(IPC_PPG_WAVEFORM_ADDR,sizeof(*out));
        if (seq==p->sequence && out->sequence==seq && out->magic==IPC_PPG_MAGIC && p->magic==IPC_PPG_MAGIC)
            return 1;
    }
    return 0;
}
#endif
