#!/usr/bin/env python3
"""
diag2.py — Phase 2: "还能试的" 系统过一遍

P1 已证: 67 样本/单波长/5s 窗 + raw CNN 是死路。
P2 探索: 改特征工程 + 改模型容量, 看是否能逼出过闸门的指标。

实验目录:
  B1 PPG-FFT      : 把 (128,2) raw 替成 0-5Hz FFT 前 16 bin × 2 = 32 维 + scalar(7) = 39 维 → Dense
  B2 PPG-morph    : 5 个 hand-crafted PPG features (mean/std/range/HR-est/AC-DC ratio) + scalar(7) = 12 维 → Dense
  B3 Minimal      : 极简模型 (~200 参数) — Concat 后单层 Dense(8) → Dense(1)
  B4 Target-norm  : 预测 z-score (y - subject_mean)/subject_std → 看消除 baseline 后 PPG 是否可学
                    (仅诊断, 部署不可行)
  B5 Ensemble     : A2 (scalar-only) + A++ 加权集成, 找最佳权重
  B6 Sample-weight: 高血糖 (y>7) subject 加权 2x

预算: B1-B6 各 5fold × 1seed × 50 epoch (~5 min/实验)

用法:
  python diag2.py                  # 全跑
  python diag2.py --only b1 b3     # 选跑
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

tf = None
build_glucose_model = None
normalize_scalars = None
eval_metrics = None
train_one = None


def _ensure_deps():
    global tf, build_glucose_model, normalize_scalars, eval_metrics, train_one
    if tf is not None:
        return
    try:
        import tensorflow as _tf
        tf = _tf
    except ImportError:
        print("[err] tensorflow not installed")
        sys.exit(2)
    from model import build_glucose_model as _bm, normalize_scalars as _ns
    import train as _train_mod
    _train_mod._ensure_tf()
    build_glucose_model = _bm
    normalize_scalars = _ns
    eval_metrics = _train_mod.eval_metrics
    train_one = _train_mod.train_one


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
OUT_DIR = ROOT / "outputs"
DIAG_DIR = OUT_DIR / "diag2"


# ── 数据加载 ──────────────────────────────────────────────────────────


def load_full_data():
    src = PROC_DIR / "multi.npz"
    if not src.exists():
        src = PROC_DIR / "must.npz"
    d = np.load(src, allow_pickle=True)
    X_ppg = d["X_ppg"].astype(np.float32)
    X_scal = normalize_scalars(d["X_scal"])
    y = d["y"].astype(np.float32)
    sid = (np.asarray(d["subject_id"]).astype(str)
           if "subject_id" in d.files
           else np.array([f"auto_{i//3}" for i in range(len(y))]))
    return X_ppg, X_scal, y, sid


def trivial_baselines(y_true):
    return {
        "mean": float(np.mean(np.abs(y_true - y_true.mean()))),
        "median": float(np.mean(np.abs(y_true - np.median(y_true)))),
    }


def quick_metrics(y_true, y_pred):
    err = y_pred - y_true
    mae = float(np.mean(np.abs(err)))
    ss_res = float(np.sum(err**2))
    ss_tot = float(np.sum((y_true - y_true.mean())**2))
    r2 = float(1.0 - ss_res / max(ss_tot, 1e-9))
    if y_pred.std() < 1e-6:
        pearson = float("nan")
    else:
        pearson = float(np.corrcoef(y_true, y_pred)[0, 1])
    return {
        "MAE_mmol": mae,
        "R2": r2,
        "Pearson_r": pearson,
        "pred_std_ratio": float(y_pred.std() / max(y_true.std(), 1e-9)),
        "pred_mean": float(y_pred.mean()),
        "pred_std": float(y_pred.std()),
    }


def gate_check(m, trivial_mean_mae):
    """5 维硬闸门:R²≥0.3, Pearson≥0.4, std_ratio≥0.5, gain≥20%, MAE≤1.0"""
    gain = (trivial_mean_mae - m["MAE_mmol"]) / trivial_mean_mae * 100
    g = {
        "R2_pass": m["R2"] >= 0.30,
        "Pearson_pass": (not np.isnan(m["Pearson_r"])) and m["Pearson_r"] >= 0.40,
        "std_ratio_pass": m["pred_std_ratio"] >= 0.50,
        "gain_pass": gain >= 20.0,
        "MAE_pass": m["MAE_mmol"] <= 1.0,
    }
    g["n_pass"] = sum(g.values())
    g["all_pass"] = g["n_pass"] == 5
    g["gain_pct"] = gain
    return g


# ── 特征工程 ──────────────────────────────────────────────────────────


def fft_features(X_ppg, n_bins=16, fs=100.0):
    """对每个窗口的双通道做 rfft,取前 n_bins 个频率分量幅值 → (N, n_bins*2)。"""
    N, L, C = X_ppg.shape
    feats = []
    for c in range(C):
        x = X_ppg[:, :, c]
        sp = np.abs(np.fft.rfft(x, axis=1))[:, :n_bins]  # (N, n_bins)
        feats.append(sp)
    out = np.concatenate(feats, axis=1).astype(np.float32)  # (N, n_bins*C)
    # 归一化每个 bin (跨样本 min-max)
    mn = out.min(axis=0, keepdims=True)
    mx = out.max(axis=0, keepdims=True)
    return (out - mn) / np.maximum(mx - mn, 1e-6)


def morph_features(X_ppg, fs=100.0):
    """5 个 hand-crafted PPG features (每通道):
       [0] mean, [1] std, [2] range, [3] HR estimate (peaks/sec * 60), [4] AC/DC ratio
       → 双通道 = 10 维
    """
    N, L, C = X_ppg.shape
    out = np.zeros((N, 5 * C), dtype=np.float32)
    for c in range(C):
        x = X_ppg[:, :, c]
        out[:, c*5+0] = x.mean(axis=1)
        out[:, c*5+1] = x.std(axis=1)
        out[:, c*5+2] = x.max(axis=1) - x.min(axis=1)
        # HR estimate: 简单峰检测 (大于 mean+0.5*std 的局部最大),HR = peak_count / window_sec * 60
        for i in range(N):
            xi = x[i]
            thresh = xi.mean() + 0.5 * xi.std()
            peaks = 0
            for j in range(1, L - 1):
                if xi[j] > thresh and xi[j] > xi[j-1] and xi[j] > xi[j+1]:
                    peaks += 1
            window_sec = L / fs
            out[i, c*5+3] = peaks / window_sec * 60.0 / 200.0  # 归一到 0-1 (200 bpm 上限)
        # AC/DC ratio
        dc = np.maximum(x.mean(axis=1), 1e-6)
        ac = x.std(axis=1)
        out[:, c*5+4] = np.clip(ac / dc, 0.0, 1.0)
    # 跨样本 min-max 归一化
    mn = out.min(axis=0, keepdims=True)
    mx = out.max(axis=0, keepdims=True)
    return (out - mn) / np.maximum(mx - mn, 1e-6)


# ── 极简模型 ──────────────────────────────────────────────────────────


def build_simple_model(input_dim, hidden=8, name="simple"):
    """1 个 Dense + Dropout + 1 个 Dense 回归头, ~input_dim*hidden + hidden 参数。"""
    inp = tf.keras.layers.Input(shape=(input_dim,), name="feat_in")
    h = tf.keras.layers.Dense(hidden, activation="relu", name="hidden")(inp)
    h = tf.keras.layers.Dropout(0.1, name="drop")(h)
    out = tf.keras.layers.Dense(1, activation=None, name="out")(h)
    return tf.keras.Model(inp, out, name=name)


def train_simple(X_tr, y_tr, X_va, y_va, *, epochs=50, batch=16, seed=42,
                 hidden=8, lr=1e-3):
    tf.random.set_seed(seed)
    np.random.seed(seed)
    model = build_simple_model(X_tr.shape[1], hidden=hidden)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(lr),
        loss=tf.keras.losses.Huber(delta=1.0),
        metrics=["mae"],
    )
    cbs = [
        tf.keras.callbacks.EarlyStopping(
            monitor="val_mae", mode="min", patience=15,
            restore_best_weights=True, verbose=0,
        ),
    ]
    model.fit(
        X_tr, y_tr,
        validation_data=(X_va, y_va),
        epochs=epochs, batch_size=batch, callbacks=cbs, verbose=0,
    )
    y_pred = model.predict(X_va, verbose=0).squeeze()
    n_params = model.count_params()
    tf.keras.backend.clear_session()
    return y_pred, n_params


def run_simple_cv(tag, X_feat, y, sid, *, hidden=8, epochs=50):
    """5fold × 1seed × simple model 重训。"""
    from sklearn.model_selection import GroupKFold
    gkf = GroupKFold(n_splits=5)
    all_pred, all_true = [], []
    fold_mae = []
    n_params = 0
    for fi, (idx_tr, idx_va) in enumerate(gkf.split(np.arange(len(y)), groups=sid)):
        y_pred, n_params = train_simple(
            X_feat[idx_tr], y[idx_tr],
            X_feat[idx_va], y[idx_va],
            epochs=epochs, hidden=hidden,
        )
        all_pred.append(y_pred)
        all_true.append(y[idx_va])
        mae_fi = float(np.mean(np.abs(y_pred - y[idx_va])))
        fold_mae.append(mae_fi)
        print(f"  [{tag} fold {fi}] MAE={mae_fi:.3f}")
    yp = np.concatenate(all_pred)
    yt = np.concatenate(all_true)
    m = quick_metrics(yt, yp)
    return m, n_params, yp, yt


# ════════════════════════════════════════════════════════════════════════
#  实验
# ════════════════════════════════════════════════════════════════════════


def exp_b1(X_ppg, X_scal, y, sid, *, epochs):
    """B1 — PPG FFT 频域特征替 raw 时序。"""
    print("\n" + "=" * 72)
    print(" B1 - PPG FFT (16 bins x 2 ch = 32) + scalar(7) = 39 dim -> Dense(8)")
    print("=" * 72)
    F = fft_features(X_ppg, n_bins=16, fs=100.0)
    X_feat = np.concatenate([F, X_scal], axis=1).astype(np.float32)
    print(f"  feat dim={X_feat.shape[1]}")
    m, n_p, yp, yt = run_simple_cv("b1_fft", X_feat, y, sid, hidden=8, epochs=epochs)
    print(f"\n[B1 OOF] MAE={m['MAE_mmol']:.3f} R2={m['R2']:+.3f} "
          f"Pearson={m['Pearson_r']:+.3f} std_ratio={m['pred_std_ratio']:.3f} "
          f"params={n_p}")
    return m, n_p


def exp_b2(X_ppg, X_scal, y, sid, *, epochs):
    """B2 — PPG hand-crafted morphology features."""
    print("\n" + "=" * 72)
    print(" B2 - PPG morph (5 feats x 2 ch = 10) + scalar(7) = 17 dim -> Dense(8)")
    print("=" * 72)
    print("  computing morph features...")
    M = morph_features(X_ppg, fs=100.0)
    X_feat = np.concatenate([M, X_scal], axis=1).astype(np.float32)
    print(f"  feat dim={X_feat.shape[1]}")
    m, n_p, yp, yt = run_simple_cv("b2_morph", X_feat, y, sid, hidden=8, epochs=epochs)
    print(f"\n[B2 OOF] MAE={m['MAE_mmol']:.3f} R2={m['R2']:+.3f} "
          f"Pearson={m['Pearson_r']:+.3f} std_ratio={m['pred_std_ratio']:.3f} "
          f"params={n_p}")
    return m, n_p


def exp_b3(X_ppg, X_scal, y, sid, *, epochs):
    """B3 — Minimal model: scalar only with tiny model."""
    print("\n" + "=" * 72)
    print(" B3 - Minimal scalar-only (7 dim -> Dense(4) -> Dense(1), ~50 params)")
    print("=" * 72)
    m, n_p, yp, yt = run_simple_cv("b3_min", X_scal, y, sid, hidden=4, epochs=epochs)
    print(f"\n[B3 OOF] MAE={m['MAE_mmol']:.3f} R2={m['R2']:+.3f} "
          f"Pearson={m['Pearson_r']:+.3f} std_ratio={m['pred_std_ratio']:.3f} "
          f"params={n_p}")
    return m, n_p


def exp_b4(X_ppg, X_scal, y, sid, *, epochs):
    """B4 — Target z-score normalization (diagnosis only)."""
    print("\n" + "=" * 72)
    print(" B4 - Target z-score (y -> (y-subj_mean)/subj_std) [DIAG, not deployable]")
    print("=" * 72)
    # Per-subject z-score
    subj_mean = {}
    subj_std = {}
    for sj in np.unique(sid):
        mask = sid == sj
        subj_mean[sj] = float(y[mask].mean())
        subj_std[sj] = float(max(y[mask].std(), 0.1))
    y_z = np.array([(y[i] - subj_mean[sid[i]]) / subj_std[sid[i]]
                    for i in range(len(y))], dtype=np.float32)

    # 用 raw FFT + scalar 作为特征 (与 B1 一致)
    F = fft_features(X_ppg, n_bins=16, fs=100.0)
    X_feat = np.concatenate([F, X_scal], axis=1).astype(np.float32)

    from sklearn.model_selection import GroupKFold
    gkf = GroupKFold(n_splits=5)
    all_pred_z, all_true_z = [], []
    for fi, (idx_tr, idx_va) in enumerate(gkf.split(np.arange(len(y)), groups=sid)):
        y_pred_z, _ = train_simple(
            X_feat[idx_tr], y_z[idx_tr],
            X_feat[idx_va], y_z[idx_va],
            epochs=epochs, hidden=8,
        )
        all_pred_z.append(y_pred_z)
        all_true_z.append(y_z[idx_va])
        print(f"  [b4 fold {fi}] z-MAE={np.mean(np.abs(y_pred_z - y_z[idx_va])):.3f}")
    yp_z = np.concatenate(all_pred_z)
    yt_z = np.concatenate(all_true_z)
    m_z = quick_metrics(yt_z, yp_z)
    print(f"\n[B4 z-domain OOF] z-MAE={m_z['MAE_mmol']:.3f} R2={m_z['R2']:+.3f} "
          f"Pearson={m_z['Pearson_r']:+.3f}")
    # 关键: 即使是 z-domain, R2/Pearson 是否显示 PPG 有可学信号?
    return m_z, None


def exp_b5(X_ppg, X_scal, y, sid, *, epochs):
    """B5 — A2 scalar-only + A++ full 加权集成 (用 A2 OOF 和 A++ OOF 拼)。"""
    print("\n" + "=" * 72)
    print(" B5 - Ensemble: scalar-only + A++ baseline weighted blend")
    print("=" * 72)
    # 复用现有 A++ baseline OOF
    aplus_pred_file = OUT_DIR / "ensemble_pred.npy"
    aplus_true_file = OUT_DIR / "ensemble_true.npy"
    if not aplus_pred_file.exists():
        print("[err] missing A++ baseline OOF (ensemble_pred.npy)")
        return None, None
    y_aplus = np.load(aplus_pred_file)
    y_true_aplus = np.load(aplus_true_file)

    # 跑 scalar-only baseline (与 A2 一致)
    print("  retraining scalar-only baseline...")
    from sklearn.model_selection import GroupKFold
    gkf = GroupKFold(n_splits=5)
    all_pred, all_true = [], []
    for fi, (idx_tr, idx_va) in enumerate(gkf.split(np.arange(len(y)), groups=sid)):
        y_pred, _ = train_simple(
            X_scal[idx_tr], y[idx_tr],
            X_scal[idx_va], y[idx_va],
            epochs=epochs, hidden=8,
        )
        all_pred.append(y_pred)
        all_true.append(y[idx_va])
    y_scal = np.concatenate(all_pred)
    y_true_scal = np.concatenate(all_true)

    # 校验顺序一致
    if not np.allclose(y_true_aplus, y_true_scal, atol=1e-3):
        print("[warn] A++ OOF and scalar OOF y order mismatch, skipping ensemble")
        return None, None

    # 找最佳 alpha
    print("  searching best alpha (A++ weight)...")
    best_m = None
    best_alpha = None
    for alpha in np.linspace(0, 1, 21):
        y_blend = alpha * y_aplus + (1 - alpha) * y_scal
        m = quick_metrics(y_true_aplus, y_blend)
        if best_m is None or m["MAE_mmol"] < best_m["MAE_mmol"]:
            best_m = m
            best_alpha = float(alpha)
    print(f"\n[B5 OOF] alpha={best_alpha:.2f} (A++ weight)  "
          f"MAE={best_m['MAE_mmol']:.3f} R2={best_m['R2']:+.3f} "
          f"Pearson={best_m['Pearson_r']:+.3f} std_ratio={best_m['pred_std_ratio']:.3f}")
    return best_m, best_alpha


def exp_b6(X_ppg, X_scal, y, sid, *, epochs):
    """B6 — Sample weights: 高血糖 (y>7) subject 加权 2x。"""
    print("\n" + "=" * 72)
    print(" B6 - Sample weight: high-glucose (y>7) samples 2x weight on full model")
    print("=" * 72)
    weights = np.where(y > 7.0, 2.0, 1.0).astype(np.float32)
    print(f"  high-glucose samples: {(y > 7.0).sum()}/{len(y)}")

    from sklearn.model_selection import GroupKFold
    gkf = GroupKFold(n_splits=5)
    all_pred, all_true = [], []
    n_params = 0
    for fi, (idx_tr, idx_va) in enumerate(gkf.split(np.arange(len(y)), groups=sid)):
        tf.random.set_seed(42)
        np.random.seed(42)
        model = build_glucose_model()
        n_params = model.count_params()
        model.compile(
            optimizer=tf.keras.optimizers.Adam(1e-3),
            loss=tf.keras.losses.Huber(delta=1.0),
            metrics=["mae"],
        )
        cbs = [
            tf.keras.callbacks.EarlyStopping(
                monitor="val_mae", mode="min", patience=15,
                restore_best_weights=True, verbose=0,
            ),
        ]
        model.fit(
            {"ppg_in": X_ppg[idx_tr], "scal_in": X_scal[idx_tr]},
            y[idx_tr],
            sample_weight=weights[idx_tr],
            validation_data=(
                {"ppg_in": X_ppg[idx_va], "scal_in": X_scal[idx_va]},
                y[idx_va],
            ),
            epochs=epochs, batch_size=16, callbacks=cbs, verbose=0,
        )
        y_pred = model.predict(
            {"ppg_in": X_ppg[idx_va], "scal_in": X_scal[idx_va]}, verbose=0
        ).squeeze()
        mae_fi = float(np.mean(np.abs(y_pred - y[idx_va])))
        print(f"  [b6 fold {fi}] MAE={mae_fi:.3f}")
        all_pred.append(y_pred)
        all_true.append(y[idx_va])
        tf.keras.backend.clear_session()

    yp = np.concatenate(all_pred)
    yt = np.concatenate(all_true)
    m = quick_metrics(yt, yp)
    print(f"\n[B6 OOF] MAE={m['MAE_mmol']:.3f} R2={m['R2']:+.3f} "
          f"Pearson={m['Pearson_r']:+.3f} std_ratio={m['pred_std_ratio']:.3f}")
    return m, n_params


# ════════════════════════════════════════════════════════════════════════
#  主流程
# ════════════════════════════════════════════════════════════════════════


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", nargs="+",
                        default=["b1", "b2", "b3", "b4", "b5", "b6"],
                        choices=["b1", "b2", "b3", "b4", "b5", "b6"])
    parser.add_argument("--epochs", type=int, default=50)
    args = parser.parse_args()

    _ensure_deps()
    DIAG_DIR.mkdir(parents=True, exist_ok=True)

    X_ppg, X_scal, y, sid = load_full_data()
    print(f"[data] N={len(y)} subjects={len(np.unique(sid))} "
          f"y_range=[{y.min():.2f}, {y.max():.2f}] mean={y.mean():.2f}+-{y.std():.2f}")

    tb = trivial_baselines(y)
    print(f"[trivial-mean MAE] {tb['mean']:.3f}")

    results = {}
    if "b1" in args.only:
        m, p = exp_b1(X_ppg, X_scal, y, sid, epochs=args.epochs)
        g = gate_check(m, tb["mean"])
        results["b1"] = {"metrics": m, "params": p, "gates": g}
    if "b2" in args.only:
        m, p = exp_b2(X_ppg, X_scal, y, sid, epochs=args.epochs)
        g = gate_check(m, tb["mean"])
        results["b2"] = {"metrics": m, "params": p, "gates": g}
    if "b3" in args.only:
        m, p = exp_b3(X_ppg, X_scal, y, sid, epochs=args.epochs)
        g = gate_check(m, tb["mean"])
        results["b3"] = {"metrics": m, "params": p, "gates": g}
    if "b4" in args.only:
        m, _ = exp_b4(X_ppg, X_scal, y, sid, epochs=args.epochs)
        # B4 是 z-domain, gate 在 z-domain 评 (zR2 > 0 = baseline 之上学到东西)
        results["b4"] = {"metrics_zdomain": m, "note": "z-domain only, gates N/A"}
    if "b5" in args.only:
        m, alpha = exp_b5(X_ppg, X_scal, y, sid, epochs=args.epochs)
        if m is not None:
            g = gate_check(m, tb["mean"])
            results["b5"] = {"metrics": m, "alpha": alpha, "gates": g}
    if "b6" in args.only:
        m, p = exp_b6(X_ppg, X_scal, y, sid, epochs=args.epochs)
        g = gate_check(m, tb["mean"])
        results["b6"] = {"metrics": m, "params": p, "gates": g}

    # ── 总结表 ────────────────────────────────────────────────────────
    print("\n" + "=" * 80)
    print(" Phase 2 Summary - 5维硬闸门检查 (R2>=0.3 / Pearson>=0.4 / std_ratio>=0.5 / gain>=20% / MAE<=1)")
    print("=" * 80)
    print(f"{'exp':<18} {'MAE':>6} {'R2':>7} {'Pear':>7} {'std_r':>6} {'gain%':>6} {'gates':>6} {'params':>7}")
    print("-" * 80)

    # 现有 baseline
    if (OUT_DIR / "ensemble_pred.npy").exists():
        yp = np.load(OUT_DIR / "ensemble_pred.npy")
        yt = np.load(OUT_DIR / "ensemble_true.npy")
        m = quick_metrics(yt, yp)
        g = gate_check(m, tb["mean"])
        print(f"{'A++ baseline':<18} {m['MAE_mmol']:>6.3f} {m['R2']:>+7.3f} "
              f"{m['Pearson_r']:>+7.3f} {m['pred_std_ratio']:>6.3f} {g['gain_pct']:>+6.1f} "
              f"{g['n_pass']:>3}/5  {3000:>7}")

    label_map = {
        "b1": "B1 PPG-FFT",
        "b2": "B2 PPG-morph",
        "b3": "B3 Minimal-scal",
        "b5": "B5 Ensemble",
        "b6": "B6 SampleWeight",
    }
    for tag, lbl in label_map.items():
        if tag in results and "metrics" in results[tag]:
            m = results[tag]["metrics"]
            g = results[tag]["gates"]
            p = results[tag].get("params") or results[tag].get("alpha", "-")
            p_str = f"{p:>7}" if isinstance(p, int) else f"{p:>7}"
            print(f"{lbl:<18} {m['MAE_mmol']:>6.3f} {m['R2']:>+7.3f} "
                  f"{m['Pearson_r']:>+7.3f} {m['pred_std_ratio']:>6.3f} "
                  f"{g['gain_pct']:>+6.1f} {g['n_pass']:>3}/5  {p_str}")

    if "b4" in results:
        m = results["b4"]["metrics_zdomain"]
        print(f"{'B4 z-norm (DIAG)':<18} {m['MAE_mmol']:>6.3f} {m['R2']:>+7.3f} "
              f"{m['Pearson_r']:>+7.3f} {m['pred_std_ratio']:>6.3f} {'-':>6} {'-':>6}")

    # ── 结论 ────────────────────────────────────────────────────────────
    print("\n" + "=" * 80)
    print(" Conclusion")
    print("=" * 80)
    any_pass = any(
        results[k].get("gates", {}).get("all_pass", False)
        for k in results if "gates" in results[k]
    )
    best_3 = sorted(
        [(k, results[k]["gates"]["n_pass"]) for k in results if "gates" in results[k]],
        key=lambda x: -x[1]
    )[:3]
    print(f"  any all-pass: {any_pass}")
    print(f"  top-3 by n_pass: {best_3}")
    if not any_pass:
        max_pass = max((results[k]["gates"]["n_pass"]
                       for k in results if "gates" in results[k]), default=0)
        if max_pass <= 1:
            print(f"\n  -> all experiments at most {max_pass}/5 gates pass")
            print(f"     67-sample physical ceiling confirmed across {len(results)} approaches")
        else:
            print(f"\n  -> best experiment gets {max_pass}/5 gates")

    # ── 保存 ──────────────────────────────────────────────────────────
    out_json = DIAG_DIR / "diag2_report.json"
    out_json.write_text(json.dumps(results, indent=2, ensure_ascii=False, default=float))
    print(f"\n[save] {out_json}")


if __name__ == "__main__":
    main()
