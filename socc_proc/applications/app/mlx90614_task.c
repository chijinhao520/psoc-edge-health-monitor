/*
 * MLX90614 sampling task
 *
 * Reads infrared body temperature at ~1Hz via TCA9548A mux CH2.
 * Data will be used for BMR calculation in later stages.
 */

#include <rtthread.h>
#include "../sensor/mlx90614.h"
#include "../sensor/tca9548a.h"
#include "sensor_data.h"
#include "ipc_shared_data.h"
#include <math.h>

#define DBG_TAG "mlx90614_task"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 拓展板 v4.3.1 设计：MLX90614 SMBus 时序特殊，独占 mux CH2 */
#define MLX_MUX_CHANNEL  2

static void mlx90614_thread_entry(void *param)
{
    RT_UNUSED(param);

    /* mux 用进程内共享单例（同 ppg/ecg/gsr），私有 init 会各建一把
     * 互不相干的锁，并发切通道时互相踩 */
    tca9548a_device_t mux = RT_NULL;
    mlx90614_device_t dev = RT_NULL;
    rt_thread_mdelay(300); /* 上电等待不占用共享总线。 */
    while (!dev) {
        if (!mux) mux=tca9548a_shared();
        if (mux && tca9548a_lock(mux)==RT_EOK) {
            if (tca9548a_select_channel(mux,MLX_MUX_CHANNEL)==RT_EOK) {
                rt_thread_mdelay(1);
                dev=mlx90614_try_init("i2c1");
            }
            tca9548a_unlock(mux);
        }
        if (!dev) rt_thread_mdelay(2000);
    }
    LOG_I("MLX90614 init success (mux CH%d), start sampling", MLX_MUX_CHANNEL);

    while (1)
    {
        float obj_temp;
        rt_err_t err;

        /* "选通道 + 访问下游设备" 必须整段包在 mux 锁里，
         * 防止 ppg(CH0/1)/ecg/gsr(CH4) 在中间插一脚改了通道 */
        if (tca9548a_lock(mux)!=RT_EOK) {
            sensor_data_update_mlx90614(NAN);
            rt_thread_mdelay(1000);
            continue;
        }
        if (tca9548a_select_channel(mux, MLX_MUX_CHANNEL) != RT_EOK)
        {
            tca9548a_unlock(mux);
            sensor_data_update_mlx90614(NAN);
            rt_thread_mdelay(1000);
            continue;
        }

        /*
         * ★ mux 切换后必须留稳定时间，否则 SMBus 读回全 0xFF。
         *
         * 2026-07-16 实测定位：同一器件、同一通道，MSH 诊断命令(mlx_diag)
         * 8/8 全部读成功，而本任务 100% 失败(PEC calc=0x22 恒定，反推正是
         * DataL=DataH=0xFF，即数据阶段完全无响应)。两者唯一差异是诊断命令
         * 在 read-word 前多做了一次单字节地址探测 —— 那次探测本身不重要，
         * 它提供的"间隔"才是关键。
         *
         * 为什么以前不发作：ppg_task 慢速采样时 mux 常常已停在 CH2 上(无需
         * 切换)；把 PPG 采样率从 51Hz 提到 88Hz 后 mux 几乎每次都是刚从
         * CH0/CH1 切过来，每次都撞上通道未稳定。
         *
         * 只有 MLX 需要这个延时：MAX30102/ADS1015 走标准 I2C、时序宽松，
         * MLX90614 走 SMBus，对时序最敏感。本任务 1Hz，1ms 延时可忽略。
         */
        rt_thread_mdelay(1);

        /*
         * 重试：1ms 稳定时间把成功率从 4% 拉到 84%，剩余约 16% 的偶发失败
         * 是切换后仍未完全稳定(总线竞争让实际间隔波动)。本任务 1Hz，
         * 重试 2 次、每次多等 2ms 的代价可忽略，换到接近 100% 的可用性。
         */
        {
            int attempt;

            for (attempt = 0; attempt < 3; attempt++)
            {
                err = mlx90614_read_obj_temp(dev, &obj_temp);
                if (err == RT_EOK)
                    break;
                rt_thread_mdelay(2);
            }
        }
        tca9548a_unlock(mux);

        if (err == RT_EOK)
        {
            sensor_data_update_mlx90614(obj_temp);
            LOG_I("ObjTemp: %.2f C", obj_temp);


        }
        else
        {
            sensor_data_update_mlx90614(NAN);
            /* 3 次重试仍失败才报。偶发单次失败在共享 mux 上属正常，
             * 每次都刷 Warning 会淹没真正的问题（此前满屏 PEC 失败正是如此） */
            LOG_W("MLX90614 read failed after 3 retries, check sensor connection");
        }

        rt_thread_mdelay(1000);
    }
}

int mlx90614_task_init(void)
{
    /*
     * 优先级 10：必须**高于** ppg(12)。
     *
     * 软 I2C 是 bit-bang，时序全靠 CPU 精确延时撑着，读取过程中被抢占就会
     * 时序错乱。MLX90614 走 SMBus，一次 read-word 要连读 3 字节
     * (DataL/DataH/PEC) 且中间夹 repeated start，这段尤其经不起打断。
     *
     * 2026-07-16 实测：ppg_task 把采样 delay 从 10ms 压到 2ms(为把采样率
     * 拉回 ~88Hz)后，几乎不让出 CPU；MLX 原优先级 14 低于 ppg(12)，
     * 每次读到一半就被抢走，后续字节读成 0xFF —— PEC 失败率高达 96%
     * (46 失败 / 2 成功)，症状与 memory 记载的"高速下第 2、3 字节读 0xFF"
     * 相同，但根因是被抢占而非总线过快(delay_us 仍为 5 = 100kHz)。
     *
     * 让 MLX 抢占 ppg 的代价极小：本任务 1Hz、每次仅几毫秒 bit-bang，
     * 对 ppg 采样率的影响 <0.5%；反之若靠调低 ppg 采样率来迁就，
     * 会让 PPG 波形偏离血糖模型训练时的 100Hz 口径，代价大得多。
     *
     * 优先级排布：ecg(11) 与本任务同属"不可被打断的 I2C 读"，均高于 ppg(12)。
     */
    rt_thread_t tid = rt_thread_create(
        "mlx906",
        mlx90614_thread_entry,
        RT_NULL,
        1024,
        10,
        10);

    if (tid == RT_NULL)
    {
        LOG_E("MLX90614 thread create failed");
        return -RT_ERROR;
    }

    rt_thread_startup(tid);
    return RT_EOK;
}
/* [产品决策 2026-07-16] 重新启用：MLX90614 已接到拓展板 mux CH2（SMBus 独占
 * 通道，i2c_scan 实测 0x5A ACK），体温回归产品参数，撤销 07-09 的停用决策。
 * bmr_task 的占位逻辑已同步回退为真实 t_ir / mlx_valid 路径。 */
INIT_APP_EXPORT(mlx90614_task_init);
