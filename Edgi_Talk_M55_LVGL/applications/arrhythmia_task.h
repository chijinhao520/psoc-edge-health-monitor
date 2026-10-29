/*
 * arrhythmia_task.h — M55 心律不齐推理任务接口(主 AI ①)
 *
 * 数据流: AD8232 ECG(500Hz) → [M33 ADS1015 采集 → ECG-IPC] → 本任务
 *           → arr_preproc(R 峰检测+切窗+R-R) → NPU 推理 → 结果存本地 static
 *
 * 结果存 M55 本地 static(同核读写,不写回 IPC)。
 * ui_data 通过 arrhythmia_get_result() 直接读(同核,无需 IPC)。
 *
 * ECG-IPC 已接通；输入采样率由 M33 实测并随 stream_epoch 重建连续窗口。
 */

#ifndef __ARRHYTHMIA_TASK_H__
#define __ARRHYTHMIA_TASK_H__

#include <stdint.h>

#define ARR_SRC_NPU       0
#define ARR_SRC_FALLBACK  1

typedef struct {
    int      last_class;     /* 最近一拍类别 0=N 1=S 2=V 3=F */
    float    last_conf;      /* 该类置信度 [0,1](fallback 为 0) */
    float    probs[4];       /* 4 类概率 */
    uint32_t beat_count;     /* 累计分类心拍数 */
    uint32_t cls_count[4];   /* 各类累计 */
    uint32_t pvc_per_min;    /* 近 1 分钟室早(V)数,PVC 负荷 */
    int      pvc_valid;      /* 至少一个完整连续统计窗后才有效 */
    int      source;         /* ARR_SRC_NPU / ARR_SRC_FALLBACK */
    int      valid;          /* 1 = 至少分类过一拍 */
    uint32_t tick;           /* 最近更新时间戳 */
    uint32_t session_id;     /* 本机引导体检的重建窗口确认号 */
} arr_result_t;

/** @brief 获取最新结果(指向 static,始终有效;valid==0 表示尚无心拍)。*/
const arr_result_t *arrhythmia_get_result(void);
void arrhythmia_copy_result(arr_result_t *out);
/* 非阻塞请求；算法线程清窗并跳过旧环形数据后，结果携带返回的确认号。 */
uint32_t arrhythmia_request_session(void);

/**
 * @brief 喂入一段 AD8232 ECG 采样(500Hz)。集成点。
 *
 * 未来 ECG-IPC reader(从 M33 ADS1015 取数)按块调用本函数即可驱动整链。
 * 内部: 逐样本 arr_preproc_push_500 → 满一拍 → NPU(L1)/规则(L3) → 更新结果。
 *
 * @param samples 500Hz ECG 采样数组
 * @param n       样本数
 * @return 本次产出并分类的心拍数
 */
int arrhythmia_feed_500hz(const float *samples, int n);

#endif /* __ARRHYTHMIA_TASK_H__ */
