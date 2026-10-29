#!/usr/bin/env python3
"""
augment.py — PPG-glucose 训练数据增强工厂

四种增强（A++ 精准化策略维度 3）：
  1. Mixup(α=0.2)        — 批内 Beta 采样混合系数，PPG/scalar/label 同步线性插值
  2. Time Warping        — 时间轴随机 ±10% 拉伸 + 重采样回 128 点
  3. Magnitude Warping   — sigmoid/cubic spline 曲线幅值扰动 ±5%
  4. Baseline drift      — 0.1-0.5 Hz 正弦低频干扰，±0.05 幅值（模拟接触不良）

设计原则：
  - 仅用于训练管线；验证/评估/量化校准集保持纯净（调用方决定）
  - 输入约定 dtype float32；shape: PPG (B, 128, 2), scalar (B, 7), y (B,)
  - 数值范围：PPG ∈ [0, 1]（min-max 归一化后），增强可能短暂越界，函数自动 clip 回 [0, 1]
  - 标量 scalar 已被 normalize_scalars 处理到 [0, 1]，增强只对 PPG / label / 极少数 scalar mix

接口：
  apply_mixup(ppg, scal, y, alpha=0.2)
      → (ppg_mix, scal_mix, y_mix)
  apply_time_warp(ppg, max_warp=0.1)
      → ppg_warped
  apply_mag_warp(ppg, max_strength=0.05, n_knots=4)
      → ppg_warped
  apply_baseline_drift(ppg, max_amp=0.05, fs=100, freq_range=(0.1, 0.5))
      → ppg_drifted

  build_augment_fn(mixup=True, time_warp=True, mag_warp=True, baseline_drift=True,
                   mixup_alpha=0.2, ...) → callable
      返回适用于 tf.data 的 map 函数（接 (ppg, scal, y) → 增强后 (ppg, scal, y)）

用法（独立运行单测）：
    python augment.py
"""

from __future__ import annotations

import numpy as np
from scipy import interpolate
from scipy import signal as sp_signal

# 常量
PPG_LEN = 128
PPG_CH = 2
SCAL_DIM = 7


# ─── 单独增强函数 ────────────────────────────────────────────────────


def apply_mixup(
    ppg: np.ndarray,
    scal: np.ndarray,
    y: np.ndarray,
    alpha: float = 0.2,
    rng: np.random.Generator | None = None,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Mixup：批内随机两样本线性插值。

    λ ~ Beta(α, α)；返回 λ·x_i + (1-λ)·x_j，PPG/scalar/label 同步混合。

    Args:
        ppg: (B, 128, 2) float32
        scal: (B, 7) float32
        y: (B,) float32
        alpha: Beta 分布参数（论文经验值 0.2，过大会破坏标签语义）
        rng: numpy Generator，方便测试时固定 seed
    """
    rng = rng or np.random.default_rng()
    B = ppg.shape[0]
    if B < 2:
        return ppg, scal, y  # 单样本跳过

    # λ ~ Beta(α, α)
    lam = rng.beta(alpha, alpha, size=B).astype(np.float32)
    # 强制 λ ≥ 0.5 提升训练稳定（避免标签翻转）
    lam = np.maximum(lam, 1.0 - lam)

    # 随机配对（shuffle 一遍取对应位置）
    perm = rng.permutation(B)
    lam_p = lam.reshape(B, 1, 1)
    lam_s = lam.reshape(B, 1)
    lam_y = lam

    ppg_mix = lam_p * ppg + (1.0 - lam_p) * ppg[perm]
    scal_mix = lam_s * scal + (1.0 - lam_s) * scal[perm]
    y_mix = lam_y * y + (1.0 - lam_y) * y[perm]

    return (
        np.clip(ppg_mix, 0.0, 1.0).astype(np.float32),
        np.clip(scal_mix, 0.0, 1.0).astype(np.float32),
        y_mix.astype(np.float32),
    )


def apply_time_warp(
    ppg: np.ndarray,
    max_warp: float = 0.10,
    rng: np.random.Generator | None = None,
) -> np.ndarray:
    """Time warping：时间轴随机 ±max_warp 拉伸/压缩，再重采样回 128 点。

    每个样本独立采一个 warp 系数。

    Args:
        ppg: (B, 128, 2) float32 或 (128, 2) 单样本
        max_warp: 最大拉伸率（0.1 = ±10%）
    """
    rng = rng or np.random.default_rng()
    single = ppg.ndim == 2
    if single:
        ppg = ppg[None, ...]
    B = ppg.shape[0]
    out = np.empty_like(ppg)
    for i in range(B):
        factor = 1.0 + rng.uniform(-max_warp, max_warp)
        new_len = max(int(round(PPG_LEN * factor)), 16)
        for c in range(PPG_CH):
            resampled = sp_signal.resample(ppg[i, :, c], new_len)
            # 再重采样回 128
            out[i, :, c] = sp_signal.resample(resampled, PPG_LEN)
    out = np.clip(out, 0.0, 1.0).astype(np.float32)
    return out[0] if single else out


def apply_mag_warp(
    ppg: np.ndarray,
    max_strength: float = 0.05,
    n_knots: int = 4,
    rng: np.random.Generator | None = None,
) -> np.ndarray:
    """Magnitude warping：cubic spline 控制点幅值扰动。

    在 n_knots 个均匀分布的控制点处采样幅值倍数 ~ N(1, max_strength)，
    用 cubic spline 在时间轴上插值，乘到 PPG 上。

    Args:
        ppg: (B, 128, 2) 或 (128, 2)
        max_strength: 倍数标准差（0.05 = ±5%）
    """
    rng = rng or np.random.default_rng()
    single = ppg.ndim == 2
    if single:
        ppg = ppg[None, ...]
    B = ppg.shape[0]
    knot_xs = np.linspace(0, PPG_LEN - 1, n_knots)
    full_xs = np.arange(PPG_LEN)
    out = ppg.copy()
    for i in range(B):
        for c in range(PPG_CH):
            knot_ys = rng.normal(1.0, max_strength, size=n_knots).astype(np.float32)
            spline = interpolate.CubicSpline(knot_xs, knot_ys)
            warp_curve = spline(full_xs).astype(np.float32)
            out[i, :, c] = out[i, :, c] * warp_curve
    out = np.clip(out, 0.0, 1.0).astype(np.float32)
    return out[0] if single else out


def apply_baseline_drift(
    ppg: np.ndarray,
    max_amp: float = 0.05,
    fs: int = 100,
    freq_range: tuple[float, float] = (0.1, 0.5),
    rng: np.random.Generator | None = None,
) -> np.ndarray:
    """模拟接触不良低频基线漂移：单一正弦在 freq_range 内随机频率 + 随机相位。

    Args:
        ppg: (B, 128, 2) 或 (128, 2)
        max_amp: 漂移幅值上限（0.05 = ±5%）
        fs: 采样率 Hz
        freq_range: 漂移频率范围 Hz
    """
    rng = rng or np.random.default_rng()
    single = ppg.ndim == 2
    if single:
        ppg = ppg[None, ...]
    B = ppg.shape[0]
    t = np.arange(PPG_LEN) / fs
    out = ppg.copy()
    for i in range(B):
        freq = rng.uniform(*freq_range)
        phase = rng.uniform(0, 2 * np.pi)
        amp = rng.uniform(0, max_amp)
        drift = (amp * np.sin(2 * np.pi * freq * t + phase)).astype(np.float32)
        # 两通道相同漂移（同源传感器）
        for c in range(PPG_CH):
            out[i, :, c] = out[i, :, c] + drift
    out = np.clip(out, 0.0, 1.0).astype(np.float32)
    return out[0] if single else out


# ─── 组合工厂 ────────────────────────────────────────────────────────


def build_augment_fn(
    mixup: bool = True,
    time_warp: bool = True,
    mag_warp: bool = True,
    baseline_drift: bool = True,
    mixup_alpha: float = 0.2,
    time_warp_max: float = 0.10,
    mag_warp_max: float = 0.05,
    baseline_drift_max: float = 0.05,
    seed: int | None = None,
):
    """工厂：返回一个 callable(ppg, scal, y) → (ppg', scal', y') 用于 numpy 训练循环。

    顺序：time_warp → mag_warp → baseline_drift → mixup（mixup 必须最后，否则破坏配对）
    """
    rng = np.random.default_rng(seed)

    def _augment(ppg: np.ndarray, scal: np.ndarray, y: np.ndarray):
        out_p, out_s, out_y = ppg.astype(np.float32), scal.astype(np.float32), y.astype(np.float32)
        if time_warp:
            out_p = apply_time_warp(out_p, time_warp_max, rng=rng)
        if mag_warp:
            out_p = apply_mag_warp(out_p, mag_warp_max, rng=rng)
        if baseline_drift:
            out_p = apply_baseline_drift(out_p, baseline_drift_max, rng=rng)
        if mixup:
            out_p, out_s, out_y = apply_mixup(out_p, out_s, out_y, mixup_alpha, rng=rng)
        return out_p, out_s, out_y

    return _augment


# ─── 等级化 CLI 入口（--aug 0..4 控制级别） ──────────────────────────


def get_augment_fn_by_level(level: int, seed: int | None = None):
    """级别化增强（用于 train.py --aug N）。

    级别 0: 无增强（baseline）
    级别 1: 仅 time_warp
    级别 2: 1 + mag_warp
    级别 3: 2 + baseline_drift
    级别 4: 3 + mixup（A++ 默认）
    """
    configs = {
        0: dict(mixup=False, time_warp=False, mag_warp=False, baseline_drift=False),
        1: dict(mixup=False, time_warp=True, mag_warp=False, baseline_drift=False),
        2: dict(mixup=False, time_warp=True, mag_warp=True, baseline_drift=False),
        3: dict(mixup=False, time_warp=True, mag_warp=True, baseline_drift=True),
        4: dict(mixup=True, time_warp=True, mag_warp=True, baseline_drift=True),
    }
    if level not in configs:
        raise ValueError(f"augment level 必须在 [0,4]，得到 {level}")
    if level == 0:
        return lambda p, s, y: (p.astype(np.float32), s.astype(np.float32), y.astype(np.float32))
    return build_augment_fn(**configs[level], seed=seed)


# ─── 单测 ────────────────────────────────────────────────────────────


def _test_all():
    """单测主入口。无需 pytest，直接 python augment.py。"""
    print("=" * 60)
    print(" augment.py 单测")
    print("=" * 60)

    rng = np.random.default_rng(42)
    B = 8
    ppg = rng.uniform(0.2, 0.8, size=(B, PPG_LEN, PPG_CH)).astype(np.float32)
    scal = rng.uniform(0.3, 0.7, size=(B, SCAL_DIM)).astype(np.float32)
    y = rng.uniform(4.0, 12.0, size=(B,)).astype(np.float32)

    n_pass = 0
    n_total = 0

    def assert_eq(name, cond, detail=""):
        nonlocal n_pass, n_total
        n_total += 1
        if cond:
            n_pass += 1
            print(f"  [ok]   {name} {detail}")
        else:
            print(f"  [FAIL] {name} {detail}")

    # T1: Mixup
    print("\n[T1] Mixup")
    p1, s1, y1 = apply_mixup(ppg, scal, y, alpha=0.2, rng=rng)
    assert_eq("Mixup ppg shape", p1.shape == ppg.shape, f"got {p1.shape}")
    assert_eq("Mixup scal shape", s1.shape == scal.shape, f"got {s1.shape}")
    assert_eq("Mixup y shape", y1.shape == y.shape, f"got {y1.shape}")
    assert_eq("Mixup ppg in [0,1]", float(p1.min()) >= 0.0 and float(p1.max()) <= 1.0)
    assert_eq("Mixup y 仍在合理范围", float(y1.min()) >= 2.0 and float(y1.max()) <= 20.0)

    # T2: Time warp
    print("\n[T2] Time warp")
    p2 = apply_time_warp(ppg, max_warp=0.10, rng=rng)
    assert_eq("TimeWarp ppg shape", p2.shape == ppg.shape, f"got {p2.shape}")
    assert_eq("TimeWarp ppg in [0,1]", float(p2.min()) >= 0.0 and float(p2.max()) <= 1.0)
    assert_eq("TimeWarp 实际改变了信号", not np.allclose(p2, ppg, atol=1e-4))

    # T3: Mag warp
    print("\n[T3] Mag warp")
    p3 = apply_mag_warp(ppg, max_strength=0.05, n_knots=4, rng=rng)
    assert_eq("MagWarp ppg shape", p3.shape == ppg.shape, f"got {p3.shape}")
    assert_eq("MagWarp ppg in [0,1]", float(p3.min()) >= 0.0 and float(p3.max()) <= 1.0)
    rel_diff = float(np.abs(p3 - ppg).mean())
    assert_eq("MagWarp 平均扰动幅度合理 (<10%)", rel_diff < 0.10, f"diff={rel_diff:.4f}")

    # T4: Baseline drift
    print("\n[T4] Baseline drift")
    p4 = apply_baseline_drift(ppg, max_amp=0.05, rng=rng)
    assert_eq("Drift ppg shape", p4.shape == ppg.shape, f"got {p4.shape}")
    assert_eq("Drift ppg in [0,1]", float(p4.min()) >= 0.0 and float(p4.max()) <= 1.0)
    assert_eq("Drift 实际改变了信号", not np.allclose(p4, ppg, atol=1e-4))

    # T5: build_augment_fn
    print("\n[T5] build_augment_fn 组合")
    fn = build_augment_fn(seed=123)
    p5, s5, y5 = fn(ppg, scal, y)
    assert_eq("Combined shape ppg", p5.shape == ppg.shape)
    assert_eq("Combined shape scal", s5.shape == scal.shape)
    assert_eq("Combined shape y", y5.shape == y.shape)
    assert_eq("Combined ppg in [0,1]", float(p5.min()) >= 0.0 and float(p5.max()) <= 1.0)

    # T6: 级别化
    print("\n[T6] get_augment_fn_by_level")
    for lvl in [0, 1, 2, 3, 4]:
        fn = get_augment_fn_by_level(lvl, seed=lvl)
        p, s, yy = fn(ppg, scal, y)
        assert_eq(
            f"Level {lvl}",
            p.shape == ppg.shape and s.shape == scal.shape and yy.shape == y.shape,
        )

    # T7: 单样本兼容
    print("\n[T7] 单样本（2D）兼容")
    p1d = ppg[0]
    p_warp = apply_time_warp(p1d)
    assert_eq("单样本 TimeWarp shape", p_warp.shape == p1d.shape, f"got {p_warp.shape}")

    # T8: B=1 mixup 跳过
    print("\n[T8] B=1 Mixup 应跳过")
    p1, s1, y1 = apply_mixup(ppg[:1], scal[:1], y[:1])
    assert_eq("B=1 ppg 不变", np.allclose(p1, ppg[:1]))

    print("\n" + "=" * 60)
    print(f" 结果: {n_pass}/{n_total} 通过")
    print("=" * 60)
    return 0 if n_pass == n_total else 1


if __name__ == "__main__":
    import sys
    sys.exit(_test_all())
