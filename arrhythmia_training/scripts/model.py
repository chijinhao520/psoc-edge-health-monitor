#!/usr/bin/env python3
"""
model.py — 心律不齐双分支 CNN 分类模型（Vela / Ethos-U55 兼容）

单导联 ECG 心拍 (256,1) + R-R 间期特征 (3,) → 5 类 (N/S/V/F/Q) softmax。

  R-R 特征对 S 类(房性早搏=提前出现)是决定性的：单看波形房性拍≈正常拍，
  区别在时序(pre-RR 短)。复用 glucose 的双分支结构。

架构：
   ECG (256,1) ─► Reshape(256,1,1)
              ─► conv1 (16,k7,s2)+BN+ReLU → conv2 (32,k5,s2) → conv3 (32,k3,s2)
              ─► GAP                                          → 32-d ┐
                                                                     ▼
   R-R (3,)    ─► Dense(8) ReLU                              → 8-d ─►Concat(40)
                                                                     │
                                                       Dense(32) ReLU + Dropout
                                                                     │
                                                       Dense(n_cls) softmax

Vela / Ethos-U55 兼容（Conv2D((k,1)) 模拟 Conv1D，与 glucose 模型同套路）：
  - Conv2D, FullyConnected(Dense), ReLU, BatchNorm(fused), GAP, Concatenate
  - **不用** Conv1D / GRU / LSTM / Transformer
  - Softmax：若 Vela 报不兼容则该算子回退 CPU(仅几类,开销忽略)

参数预算：≤ 20000（主 AI；int8 后 ~10-20KB 权重，NPU 绰绰有余）
"""

from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers, Model, Input

# ── 配置常量 ──────────────────────────────────────────────────────────
ECG_LEN = 256
ECG_CH = 1
RR_DIM = 3
N_CLASSES = 5

CONV_FILTERS = [16, 32, 32]
CONV_KERNELS = [7, 5, 3]
CONV_STRIDE = 2
DENSE_RR = 8
DENSE_FUSE = 32
DROPOUT = 0.20

PARAM_BUDGET = 20000


def build_arrhythmia_model(
    ecg_len: int = ECG_LEN,
    ecg_ch: int = ECG_CH,
    rr_dim: int = RR_DIM,
    n_classes: int = N_CLASSES,
    name: str = "arrhythmia_cnn",
) -> Model:
    """构建双分支(ECG 波形 + R-R 间期) ECG 心拍分类 CNN。"""
    ecg_in = Input(shape=(ecg_len, ecg_ch), name="ecg_in")
    rr_in = Input(shape=(rr_dim,), name="rr_in")

    # ── ECG 波形分支 ──────────────────────────────────────────────
    x = layers.Reshape((ecg_len, 1, ecg_ch), name="ecg_reshape")(ecg_in)
    for i, (f, k) in enumerate(zip(CONV_FILTERS, CONV_KERNELS)):
        x = layers.Conv2D(
            f, (k, 1), strides=(CONV_STRIDE, 1), padding="same",
            activation=None, name=f"conv{i+1}",
        )(x)
        x = layers.BatchNormalization(name=f"conv{i+1}_bn")(x)
        x = layers.ReLU(name=f"conv{i+1}_relu")(x)
    ecg_feat = layers.GlobalAveragePooling2D(name="gap")(x)

    # ── R-R 间期标量分支 ──────────────────────────────────────────
    rr_feat = layers.Dense(DENSE_RR, activation="relu", name="rr_dense")(rr_in)

    # ── 融合 ──────────────────────────────────────────────────────
    fused = layers.Concatenate(name="concat")([ecg_feat, rr_feat])
    f = layers.Dense(DENSE_FUSE, activation="relu", name="fuse_dense")(fused)
    f = layers.Dropout(DROPOUT, name="fuse_drop")(f)
    out = layers.Dense(n_classes, activation="softmax", name="cls_out")(f)

    return Model(inputs=[ecg_in, rr_in], outputs=out, name=name)


# ── 自检 ──────────────────────────────────────────────────────────────
def _check():
    m = build_arrhythmia_model()
    m.summary(line_length=100)
    n = m.count_params()
    print(f"\nTotal params: {n}  (budget {PARAM_BUDGET})")
    if n > PARAM_BUDGET:
        print(f"[FAIL] 参数超预算 ({n} > {PARAM_BUDGET})")
        return 1
    print(f"[ok]   参数在预算内")

    op_types = {type(l).__name__ for l in m.layers}
    bad = op_types & {"Conv1D", "GRU", "LSTM", "MultiHeadAttention"}
    if bad:
        print(f"[FAIL] 含 Vela 不友好算子: {bad}")
        return 1
    print(f"[ok]   全部算子在 Vela 白名单内")

    import numpy as np
    ecg = np.random.uniform(0, 1, (4, ECG_LEN, ECG_CH)).astype(np.float32)
    rr = np.random.uniform(0, 1, (4, RR_DIM)).astype(np.float32)
    y = m({"ecg_in": ecg, "rr_in": rr}, training=False).numpy()
    print(f"[ok]   smoke test 通过，输出 shape={y.shape}, 行和={y.sum(1)[:2]}")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(_check())
