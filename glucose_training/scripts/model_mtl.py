#!/usr/bin/env python3
"""
model_mtl.py — C1 (Multi-Task Learning) + C4 (Physics-prior Fusion) 联合架构

P3 创新方向 (β 类: 不突破信息论上限,但提升产品/评审价值):

  PPG backbone (shared)
      |
      +--> HR head (C1): 强迫 backbone 学 PPG 真物理形态
      |
      +--> Glucose neural head: ppg_feat + scal_5d -> Dense(24) -> Dense(1)
      |
      +--> Physics expert head (C4): 7 dim scalar -> Dense(1) linear regression
      |
      +--> Adaptive gate: ppg_feat -> sigmoid -> [0,1]
      |
   glucose_out = gate * neural + (1-gate) * physics

为什么辅助任务只用 HR (不用 SpO2):
  SpO2 在 MUST 数据集全部为 98.0 (常数), 无监督信号; HR 范围 50-116 真有变化。

为什么 scalar branch 砍 HR (col 0) 和 SpO2 (col 1):
  - HR 给 HR head 当 supervision target, 不能再喂给 glucose head (否则 shortcut)
  - SpO2 是常数无用
  -> glucose neural head 拿到 5 维有效特征: BV/t_ir/t_env/h_env/BMR

部署兼容:
  此模型仅用于训练。部署阶段用蒸馏 (类似 ensemble_distill.py),
  把 MTL teacher 的 glucose_out 蒸馏到单 head student (与 model.py 同架构),
  保持 Vela 算子白名单合规 (Lambda crop 仅训练用)。
"""

from __future__ import annotations

import tensorflow as tf
from tensorflow.keras import layers, Model, Input


# ── 配置常量 (与 model.py 一致) ───────────────────────────────────────
PPG_LEN = 128
PPG_CH = 2
SCAL_DIM = 7

CONV_FILTERS = [8, 16, 16]
CONV_KERNEL = 5
CONV_STRIDE = 2
SE_RATIO = 4
DROPOUT = 0.10
DENSE_SCAL = 16
DENSE_FUSE = 24

# HR head
DENSE_HR = 8


# ── 子模块 (复用 model.py 设计) ────────────────────────────────────────


def _se_block(x, ratio: int = SE_RATIO, name: str = "se"):
    c = x.shape[-1]
    hidden = max(c // ratio, 1)
    s = layers.GlobalAveragePooling2D(name=f"{name}_gap")(x)
    s = layers.Dense(hidden, activation="relu", name=f"{name}_squeeze")(s)
    s = layers.Dense(c, activation="sigmoid", name=f"{name}_excite")(s)
    s = layers.Reshape((1, 1, c), name=f"{name}_reshape")(s)
    return layers.Multiply(name=f"{name}_scale")([x, s])


def _residual_conv(x, filters, kernel=(CONV_KERNEL, 1),
                   stride=(CONV_STRIDE, 1), name="rconv"):
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


# ── MTL 主模型 ────────────────────────────────────────────────────────


def build_mtl_model(name: str = "glucose_mtl_aplusplus") -> Model:
    """C1+C4 联合 MTL 架构。

    Inputs:
      ppg_in:  (B, 128, 2)
      scal_in: (B, 7)  -- HR/SpO2/BV/t_ir/t_env/h_env/BMR (normalized)

    Outputs:
      glucose_out: (B, 1) -- 主任务,gate-blend(neural, physics)
      hr_pred:     (B, 1) -- 辅助任务,从 PPG backbone 学 HR (normalized)
    """
    ppg_in = Input(shape=(PPG_LEN, PPG_CH), name="ppg_in")
    scal_in = Input(shape=(SCAL_DIM,), name="scal_in")

    # ── 共享 PPG backbone ────────────────────────────────────────────
    x = layers.Reshape((PPG_LEN, 1, PPG_CH), name="ppg_reshape")(ppg_in)
    x = layers.Conv2D(
        CONV_FILTERS[0], (CONV_KERNEL, 1), strides=(CONV_STRIDE, 1),
        padding="same", name="conv1",
    )(x)
    x = layers.BatchNormalization(name="conv1_bn")(x)
    x = layers.ReLU(name="conv1_relu")(x)
    x = _residual_conv(x, CONV_FILTERS[1], name="conv2")
    x = _residual_conv(x, CONV_FILTERS[2], name="conv3")
    x = _se_block(x, ratio=SE_RATIO, name="se")
    ppg_feat = layers.GlobalAveragePooling2D(name="ppg_gap")(x)  # (B, 16)

    # ── HR aux head (C1: 强迫 backbone 学 PPG 物理特征) ───────────────
    h = layers.Dense(DENSE_HR, activation="relu", name="hr_dense")(ppg_feat)
    h = layers.Dropout(DROPOUT, name="hr_drop")(h)
    y_hr = layers.Dense(1, activation=None, name="hr_pred")(h)

    # ── Scalar branch (砍 col 0 HR + col 1 SpO2,只用 BV/t_ir/t_env/h_env/BMR) ──
    # 训练 Lambda 部署再蒸馏到单 head 单输入,所以这里 Lambda 没问题
    scal_5d = layers.Lambda(
        lambda t: t[:, 2:], name="scal_crop_5d",
        output_shape=(SCAL_DIM - 2,),
    )(scal_in)
    s = layers.Dense(DENSE_SCAL, activation="relu", name="scal_dense")(scal_5d)
    s = layers.Dropout(DROPOUT, name="scal_drop")(s)
    scal_feat = s  # (B, 16)

    # ── Glucose neural head ──────────────────────────────────────────
    fused = layers.Concatenate(name="concat")([ppg_feat, scal_feat])  # (B, 32)
    f = layers.Dense(DENSE_FUSE, activation="relu", name="fuse_dense")(fused)
    f = layers.Dropout(DROPOUT, name="fuse_drop")(f)
    y_neural = layers.Dense(1, activation=None, name="glucose_neural")(f)

    # ── Physics expert head (C4: 7 维线性 baseline) ──────────────────
    # 直接用全 7 维 scalar 做线性回归 → 一个 Dense(1) layer 就是
    # y_phys = w_HR*HR_norm + w_SpO2*SpO2_norm + ... + w_BMR*BMR_norm + b
    y_physics = layers.Dense(
        1, activation=None, use_bias=True,
        kernel_initializer=tf.keras.initializers.RandomNormal(mean=0.0, stddev=0.1),
        bias_initializer=tf.keras.initializers.Constant(6.0 / 1.0),  # 中位先验
        name="glucose_physics",
    )(scal_in)

    # ── Adaptive gate (PPG-driven blend ratio) ───────────────────────
    gate = layers.Dense(
        1, activation="sigmoid",
        kernel_initializer=tf.keras.initializers.Zeros(),
        bias_initializer=tf.keras.initializers.Constant(0.0),  # init 0.5
        name="gate",
    )(ppg_feat)  # (B, 1) in [0, 1]

    # ── 加权融合 ─────────────────────────────────────────────────────
    one_minus_gate = layers.Lambda(
        lambda t: 1.0 - t, name="inv_gate", output_shape=(1,),
    )(gate)
    weighted_neural = layers.Multiply(name="w_neural")([gate, y_neural])
    weighted_physics = layers.Multiply(name="w_physics")([one_minus_gate, y_physics])
    y_final = layers.Add(name="glucose_out")([weighted_neural, weighted_physics])

    return Model(
        inputs=[ppg_in, scal_in],
        outputs=[y_final, y_hr],
        name=name,
    )


# ── 标量归一化 (与 model.py 一致) ─────────────────────────────────────


def normalize_scalars(X_scal):
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
    m = build_mtl_model()
    m.summary(line_length=110)
    n_params = m.count_params()
    print(f"\nTotal MTL params: {n_params}")
    print(f"vs A++ baseline (~3000 params)")

    import numpy as np
    ppg = np.random.uniform(0, 1, (2, PPG_LEN, PPG_CH)).astype(np.float32)
    scal = np.random.uniform(0, 1, (2, SCAL_DIM)).astype(np.float32)
    outs = m({"ppg_in": ppg, "scal_in": scal}, training=False)
    print(f"\n[ok] outputs:")
    print(f"  glucose_out: shape={outs[0].shape}, sample={outs[0].numpy()[0][0]:.3f}")
    print(f"  hr_pred:     shape={outs[1].shape}, sample={outs[1].numpy()[0][0]:.3f}")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(_check())
