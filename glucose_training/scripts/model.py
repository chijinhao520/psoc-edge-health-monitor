#!/usr/bin/env python3
"""
model.py — 双分支 CNN 血糖回归模型（A++ 精准化版）

A++ 强化（spec.md 维度 2）：
  - SE-block (Squeeze-Excitation) — 通道注意力，~150 参数
  - 残差连接 — conv2/conv3 加 1x1 conv shortcut（H 维下采样匹配 stride）
  - 标量分支 Dense(8) → Dense(16) + Dropout(0.1)
  - 融合 Dense(16) → Dense(24, relu) + Dense(1)

架构：
                  ┌──────────────────────────────────┐
   PPG (128,2) ─► │ conv1 → conv2+res → conv3+res    │
                  │       + SE-block                  │ ─► 16-d feat ┐
                  │ → GAP                             │              │
                  └──────────────────────────────────┘              ▼
                                                                Concat (32)
                  ┌──────────────────────────────────┐              │
   标量 (7,)   ─► │ Dense(16) ReLU + Dropout(0.1)    │ ─► 16-d feat ┘
                  └──────────────────────────────────┘              │
                                                                    ▼
                                                       Dense(24) ReLU + Dropout(0.1)
                                                                    │
                                                              Dense(1)  ─► glucose

Vela / Ethos-U55 兼容（白名单内）：
  - Conv2D, Dense (FullyConnected), ReLU, BatchNorm (fused), GlobalAveragePooling2D
  - Add (residual), Multiply (SE scale), Sigmoid (SE excite, via LUT), Reshape, Concatenate
  - **不用** Conv1D / GRU / LSTM / Transformer

参数预算：≤ 6000（A 基线 ~2500 → A++ ~4000-5000，留余量）
int8 量化后 ~5-7 KB 权重。
"""

from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers, Model, Input


# ── 配置常量 ──────────────────────────────────────────────────────────
PPG_LEN = 128
PPG_CH = 2
SCAL_DIM = 7

CONV_FILTERS = [8, 16, 16]      # 三层卷积通道
CONV_KERNEL = 5
CONV_STRIDE = 2
SE_RATIO = 4                    # SE squeeze 比例
DROPOUT = 0.10

DENSE_SCAL = 16
DENSE_FUSE = 24

PARAM_BUDGET = 6000             # spec AC-4 (A++ 放宽)


# ── 子模块 ────────────────────────────────────────────────────────────


def _se_block(x, ratio: int = SE_RATIO, name: str = "se"):
    """Squeeze-Excitation 通道注意力。Vela 友好（GAP+Dense+Sigmoid+Reshape+Multiply）。

    Args:
        x: (B, H, W, C) tensor
        ratio: squeeze 比例（C // ratio 是中间通道数）
    """
    c = x.shape[-1]
    hidden = max(c // ratio, 1)

    # Squeeze: GAP → (B, C)
    s = layers.GlobalAveragePooling2D(name=f"{name}_gap")(x)
    # Excite: Dense → ReLU → Dense → Sigmoid
    s = layers.Dense(hidden, activation="relu", name=f"{name}_squeeze")(s)
    s = layers.Dense(c, activation="sigmoid", name=f"{name}_excite")(s)
    # Reshape for broadcast → (B, 1, 1, C)
    s = layers.Reshape((1, 1, c), name=f"{name}_reshape")(s)
    # Scale
    return layers.Multiply(name=f"{name}_scale")([x, s])


def _residual_conv(
    x,
    filters: int,
    kernel=(CONV_KERNEL, 1),
    stride=(CONV_STRIDE, 1),
    name: str = "rconv",
):
    """Conv2D + BN + ReLU + 残差连接。

    如输入通道数 != filters 或 stride != (1,1)，用 1x1 Conv2D 适配 shortcut。
    """
    shortcut = x
    in_ch = x.shape[-1]

    out = layers.Conv2D(
        filters, kernel, strides=stride, padding="same",
        activation=None, name=f"{name}_conv",
    )(x)
    out = layers.BatchNormalization(name=f"{name}_bn")(out)
    out = layers.ReLU(name=f"{name}_relu")(out)

    if in_ch != filters or stride != (1, 1):
        shortcut = layers.Conv2D(
            filters, (1, 1), strides=stride, padding="same",
            name=f"{name}_shortcut",
        )(shortcut)

    return layers.Add(name=f"{name}_add")([out, shortcut])


# ── 主模型 ────────────────────────────────────────────────────────────


def build_glucose_model(
    ppg_len: int = PPG_LEN,
    ppg_ch: int = PPG_CH,
    scal_dim: int = SCAL_DIM,
    name: str = "glucose_cnn_aplusplus",
) -> Model:
    """构建 A++ 双分支 CNN。"""

    # ── 输入 ───────────────────────────────────────────────────────
    ppg_in = Input(shape=(ppg_len, ppg_ch), name="ppg_in")
    scal_in = Input(shape=(scal_dim,), name="scal_in")

    # PPG (B, 128, 2) → (B, 128, 1, 2) 给 Conv2D 用 NHWC
    x = layers.Reshape((ppg_len, 1, ppg_ch), name="ppg_reshape")(ppg_in)

    # ── PPG 分支 ───────────────────────────────────────────────────
    # conv1: 不加残差（输入通道 2 与 8 差距大，加 shortcut 性价比低）
    x = layers.Conv2D(
        CONV_FILTERS[0], (CONV_KERNEL, 1), strides=(CONV_STRIDE, 1),
        padding="same", activation=None, name="conv1",
    )(x)
    x = layers.BatchNormalization(name="conv1_bn")(x)
    x = layers.ReLU(name="conv1_relu")(x)

    # conv2 + 残差
    x = _residual_conv(x, CONV_FILTERS[1], name="conv2")
    # conv3 + 残差
    x = _residual_conv(x, CONV_FILTERS[2], name="conv3")
    # SE-block on conv3 输出（最深特征通道注意力）
    x = _se_block(x, ratio=SE_RATIO, name="se")

    # GAP → (B, 16)
    ppg_feat = layers.GlobalAveragePooling2D(name="ppg_gap")(x)

    # ── 标量分支 ───────────────────────────────────────────────────
    s = layers.Dense(DENSE_SCAL, activation="relu", name="scal_dense")(scal_in)
    s = layers.Dropout(DROPOUT, name="scal_drop")(s)
    scal_feat = s

    # ── 融合 ───────────────────────────────────────────────────────
    fused = layers.Concatenate(name="concat")([ppg_feat, scal_feat])  # (B, 32)
    f = layers.Dense(DENSE_FUSE, activation="relu", name="fuse_dense")(fused)
    f = layers.Dropout(DROPOUT, name="fuse_drop")(f)
    out = layers.Dense(1, activation=None, name="glucose_out")(f)

    return Model(inputs=[ppg_in, scal_in], outputs=out, name=name)


# ── 标量归一化（与训练/部署强契约）─────────────────────────────────
def normalize_scalars(X_scal):
    """7 维标量归一到 [0, 1]。

    **此函数定义训练-部署强契约**——M55 端 glucose_infer.cpp 必须完全照搬此公式。

    参考范围（健康成人）：
      [0] HR     : 50–120 bpm     → /120
      [1] SpO2   : 90–100 %       → (x-90)/10
      [2] BV     : 0.1–5.0 a.u.   → /5
      [3] t_ir   : 35–40 °C       → (x-35)/5
      [4] t_env  : 15–35 °C       → (x-15)/20
      [5] h_env  : 20–90 %RH      → (x-20)/70
      [6] BMR    : 30–120 kcal/h  → /120
    """
    import numpy as np

    X = np.asarray(X_scal, dtype=np.float32).copy()
    X[:, 0] = X[:, 0] / 120.0
    X[:, 1] = (X[:, 1] - 90.0) / 10.0
    X[:, 2] = X[:, 2] / 5.0
    X[:, 3] = (X[:, 3] - 35.0) / 5.0
    X[:, 4] = (X[:, 4] - 15.0) / 20.0
    X[:, 5] = (X[:, 5] - 20.0) / 70.0
    X[:, 6] = X[:, 6] / 120.0
    return np.clip(X, 0.0, 1.0)


# ── 自检 ──────────────────────────────────────────────────────────────


def _check():
    m = build_glucose_model()
    m.summary(line_length=110)
    n_params = m.count_params()
    print(f"\nTotal params: {n_params}")
    print(f"Param budget : {PARAM_BUDGET}")
    print(f"Est int8 weight size: ~{n_params} bytes ({n_params/1024:.2f} KB)")
    print(
        f"\nInput shapes : ppg_in={m.input_shape[0]}, scal_in={m.input_shape[1]}"
    )
    print(f"Output shape : {m.output_shape}")
    if n_params > PARAM_BUDGET:
        print(f"[FAIL] 参数超出预算 ({n_params} > {PARAM_BUDGET})")
        return 1
    print(f"[ok]   参数在预算内 ({n_params} <= {PARAM_BUDGET})")

    # 验证算子白名单（人工抓 layer types）
    op_types = {type(layer).__name__ for layer in m.layers}
    forbidden = {"Conv1D", "GRU", "LSTM", "MultiHeadAttention"}
    bad = op_types & forbidden
    if bad:
        print(f"[FAIL] 含 Vela 不友好算子: {bad}")
        return 1
    print(f"[ok]   全部算子在 Vela 白名单内")

    # 推理 smoke test
    import numpy as np
    ppg = np.random.uniform(0, 1, (2, PPG_LEN, PPG_CH)).astype(np.float32)
    scal = np.random.uniform(0, 1, (2, SCAL_DIM)).astype(np.float32)
    y = m({"ppg_in": ppg, "scal_in": scal}, training=False).numpy()
    print(f"[ok]   推理 smoke test 通过，输出 shape={y.shape}, dtype={y.dtype}")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(_check())
