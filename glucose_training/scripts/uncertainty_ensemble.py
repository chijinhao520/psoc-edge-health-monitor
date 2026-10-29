#!/usr/bin/env python3
"""
uncertainty_ensemble.py — P3 C2-Plus: Deep Ensemble Uncertainty

理论 (Lakshminarayanan 2017):
  集成 N 个独立训练的模型 (不同 seed/fold), 对同一样本的预测分歧 = epistemic uncertainty.
  在小数据集上通常优于 MC Dropout, 因为 MC Dropout 的噪声主要来自单模型内部参数随机,
  与真实"样本难度"相关性弱; Deep Ensemble 的分歧来自训练动态本身的随机性, 与数据难度相关性强。

本项目可用:
  5 fold x 3 seed = 15 个 best_foldF_seedS.h5 (但每个模型只在自己 fold 的 val 上没见过)

正确实现 (无泄漏):
  对每个样本 x_i, 找出它属于的 fold (val) → 只用属于其他 fold 的模型预测 x_i
  → 这样 x_i 对每个 ensemble 成员都是 hold-out
  实际上更简单的做法: x_i 在 fold F_i, 只用同 fold F_i 的 3 个 seed (因为只有 seed_0 和 seed_1
  没见过这个 fold val, 但实际我们的训练是 fold F_i 的 val 不参与该 fold 训练) →
  其实最干净的方法: 用 x_i 自己 fold 的 3 个 seed 模型 (它们都没见过 x_i)
  → ensemble size = 3 per sample
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np

tf = None
normalize_scalars = None


def _ensure_deps():
    global tf, normalize_scalars
    if tf is not None:
        return
    try:
        import tensorflow as _tf
        tf = _tf
    except ImportError:
        print("[err] tensorflow not installed")
        sys.exit(2)
    from model import normalize_scalars as _ns
    normalize_scalars = _ns


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROC_DIR = ROOT / "data" / "processed"
CKPT_DIR = ROOT / "models" / "checkpoints"
OUT_DIR = ROOT / "outputs" / "uncertainty"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    args = parser.parse_args()
    _ensure_deps()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # 加载数据
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

    # 5fold GroupKFold
    from sklearn.model_selection import GroupKFold
    splits = list(GroupKFold(n_splits=5).split(np.arange(len(y)), groups=sid))

    # 扫描所有 fold×seed checkpoints
    cks = sorted(CKPT_DIR.glob("best_fold*_seed*.h5"))
    print(f"[ckpt] {len(cks)} checkpoints found")
    fold_to_ckpts = {}
    for ck in cks:
        m = re.search(r"fold(\d+)_seed(\d+)", ck.name)
        if not m:
            continue
        fi = int(m.group(1))
        fold_to_ckpts.setdefault(fi, []).append(ck)
    for fi in sorted(fold_to_ckpts):
        print(f"  fold {fi}: {len(fold_to_ckpts[fi])} seeds")

    # 对每个样本: 它属于 fold F_i, 用 F_i 的 N seeds 预测 (均没见过 x_i)
    n = len(y)
    pred_mean_oof = np.zeros(n, dtype=np.float32)
    pred_std_oof = np.zeros(n, dtype=np.float32)

    for fi, (_, val_idx) in enumerate(splits):
        if fi not in fold_to_ckpts:
            print(f"[warn] fold {fi} has no checkpoints, skip")
            continue
        ckpts_fi = fold_to_ckpts[fi]
        per_seed_preds = []
        for ck in ckpts_fi:
            m = tf.keras.models.load_model(str(ck), compile=False)
            p = m.predict(
                {"ppg_in": X_ppg[val_idx], "scal_in": X_scal[val_idx]}, verbose=0
            ).squeeze()
            per_seed_preds.append(p)
            tf.keras.backend.clear_session()
        arr = np.stack(per_seed_preds, axis=0)  # (N_seeds, N_val)
        pred_mean_oof[val_idx] = arr.mean(axis=0)
        pred_std_oof[val_idx] = arr.std(axis=0)
        print(f"[fold {fi}] N_val={len(val_idx)} ensemble_size={len(ckpts_fi)}  "
              f"pred std mean={pred_std_oof[val_idx].mean():.4f}")

    # 校准评估
    abs_err = np.abs(pred_mean_oof - y)
    if abs_err.std() < 1e-6 or pred_std_oof.std() < 1e-6:
        corr = float("nan")
    else:
        corr = float(np.corrcoef(abs_err, pred_std_oof)[0, 1])

    # 95% CI / 50% CI
    coverage_95 = float(np.mean(
        (y >= pred_mean_oof - 1.96 * pred_std_oof) &
        (y <= pred_mean_oof + 1.96 * pred_std_oof)
    )) * 100
    coverage_50 = float(np.mean(
        (y >= pred_mean_oof - 0.674 * pred_std_oof) &
        (y <= pred_mean_oof + 0.674 * pred_std_oof)
    )) * 100

    # 按 std 分 3 桶
    sorted_idx = np.argsort(pred_std_oof)
    third = n // 3
    mae_low = float(np.mean(abs_err[sorted_idx[:third]]))
    mae_mid = float(np.mean(abs_err[sorted_idx[third:2*third]]))
    mae_hi = float(np.mean(abs_err[sorted_idx[2*third:]]))
    monotonic = mae_low < mae_mid < mae_hi

    print("\n" + "=" * 72)
    print(" Deep Ensemble Calibration (OOF)")
    print("=" * 72)
    print(f"  N_samples              : {n}")
    print(f"  ensemble std (mean)    : {pred_std_oof.mean():.4f}")
    print(f"  ensemble std (max)     : {pred_std_oof.max():.4f}")
    print(f"  abs_err vs std corr    : {corr:+.3f}  "
          f"({'CALIBRATED' if corr > 0.2 else 'WEAK' if corr > 0 else 'INVERSE'})")
    print(f"  95% CI coverage        : {coverage_95:.1f}%  (ideal ~95)")
    print(f"  50% CI coverage        : {coverage_50:.1f}%  (ideal ~50)")
    print(f"  MAE by std bucket      : low={mae_low:.3f}  "
          f"mid={mae_mid:.3f}  hi={mae_hi:.3f}")
    print(f"  monotonic (low<mid<hi) : {monotonic}")

    # 保存
    report = {
        "experiment": "P3 C2-Plus Deep Ensemble Uncertainty (OOF)",
        "n_samples": int(n),
        "ensemble_sizes_per_fold": {
            str(fi): len(fold_to_ckpts.get(fi, [])) for fi in range(5)
        },
        "pred_std_stats": {
            "mean": float(pred_std_oof.mean()),
            "min": float(pred_std_oof.min()),
            "max": float(pred_std_oof.max()),
        },
        "calibration": {
            "abs_err_vs_std_corr": corr,
            "coverage_95_pct": coverage_95,
            "coverage_50_pct": coverage_50,
            "mae_low_std": mae_low,
            "mae_mid_std": mae_mid,
            "mae_hi_std": mae_hi,
            "monotonic": monotonic,
        },
    }
    out = OUT_DIR / "deep_ensemble_uncertainty.json"
    out.write_text(json.dumps(report, indent=2, ensure_ascii=False, default=float))
    np.save(OUT_DIR / "de_pred_mean.npy", pred_mean_oof)
    np.save(OUT_DIR / "de_pred_std.npy", pred_std_oof)
    np.save(OUT_DIR / "de_y_true.npy", y)
    print(f"\n[save] {out}")


if __name__ == "__main__":
    main()
