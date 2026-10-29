"""运行实际GPU SDK初始化/失败清理函数；模拟内存分配，不访问GPU硬件。"""
import sys
from pathlib import Path
from cross_layer_sil import ROOT, OUT, compile_pc, function

sys.path.insert(0, str(ROOT / "tools/firmware"))
from sdk_patch import TARGET, check


def main():
    check()
    src = (ROOT / TARGET).read_text(encoding="utf-8")
    hdr = (ROOT / TARGET).with_name("vg_lite_platform.h").read_text(encoding="utf-8")
    types = hdr[hdr.index("typedef struct list_head"):hdr.index("typedef struct vg_module_parameters")]
    state = src[src.index("/* Implementation of list."):src.index("void vg_lite_set_gpu_execute_state")]
    c = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __attribute__(x)
#define _BAREMETAL 1
#define VG_SYSTEM_RESERVE_COUNT 3
typedef int vg_lite_vidmem_pool_t;
typedef int vg_lite_gpu_execute_state_t;
''' + types + state + r'''
static uint32_t registerMemBase, gpuMemBase[VG_SYSTEM_RESERVE_COUNT];
static uint32_t heap_size[VG_SYSTEM_RESERVE_COUNT];
static volatile void *contiguousMem[VG_SYSTEM_RESERVE_COUNT];
static int alloc_count, fail_at, live;
static int vg_lite_hal_allocate(uint32_t size, void **out) {
    *out = ++alloc_count==fail_at ? NULL : calloc(1,size);
    if(*out)live++;
    return *out?0:-1;
}
static int vg_lite_hal_free(void *p) {
    assert(p && p!=&Device && live>0);live--;free(p);return 0;
}
''' + function(src, "vg_lite_exit") + "\n" + function(src, "vg_lite_init") + r'''
static void setup(void) {
    assert(!device && !live);alloc_count=0;fail_at=-1;
    for(int i=0;i<VG_SYSTEM_RESERVE_COUNT;i++) {
        contiguousMem[i]=(void *)(uintptr_t)(0x1000+0x1000*i);
        heap_size[i]=4096;
    }
}
int main(void) {
    for(int i=0;i<VG_SYSTEM_RESERVE_COUNT;i++) {
        setup();contiguousMem[i]=NULL;
        assert(vg_lite_init()==-1 && !device && !live);
        vg_lite_exit();assert(!device && !live);
        setup();fail_at=i+1;
        assert(vg_lite_init()==-1 && !device && !live);
        vg_lite_exit();assert(!device && !live);
    }
    for(int attempt=0;attempt<2;attempt++) {
        setup();assert(vg_lite_init()==0 && device==&Device && live==VG_SYSTEM_RESERVE_COUNT);
        vg_lite_exit();assert(!device && !live);
        vg_lite_exit();assert(!device && !live);
    }
    puts("GPU cleanup: 6 partial-init failures, repeat cleanup, recovery; no static free or leaks");
    return 0;
}
'''
    OUT.mkdir(parents=True, exist_ok=True)
    print(compile_pc("gpu_cleanup_sil", c))


if __name__ == "__main__":
    main()
