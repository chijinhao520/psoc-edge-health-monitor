#!/usr/bin/env python3
"""
uncertainty.py — P3 C2: MC Dropout 不确定性建模

原理:
  推理时**不关闭 dropout**, 跑 N 次 forward, 得到 (mean, std) 作为预测的
  贝叶斯不确定性估计 (Gal & Ghahramani 2016 "Dropout as Bayesian Approximation")。

输出语义:
  - mean: glucose 点预测 (mmol/L)
  - std:  模型对该样本的"信心" — std 越大代表模型越不确定
  - 95% confidence interval = mean +/- 1.96 * std

设计要点:
  - 推理 N=30 次 (~150ms 在 M55 NPU 上, 可接受)
  - 用 functional API + training=True 强制 dropout 开启
  - 评估: 在 OOF 数据上对比"高 std 样本"是否 MAE 真的更大
    -> 如 yes, 模型不确定性是 calibrated (诚实)
    -> 如 no, dropout 噪声与样本难度无关 (不可用)

用法:
  python uncertainty.py --ckpt models/checkpoints/best_fold0_seed0.h5
  python uncertainty.py --auto  # 自动找最新的 checkpoint
"""

from __future__ import annotations

import argparse
import json
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


# ── MC Dropout 推理 ───────────────────────────────────────────────────


def mc_dropout_predict(model, ppg, scal, n_samples: int = 30):
    """N 次 dropout-open forward, 返回 (mean, std).

    关键: 用 model(..., training=True) 强制 Dropout 启用。
    BatchNorm 也会用 mini-batch 统计, 但因为 batch_size = len(全集),
    与 inference mode 行为一致, 不会引入额外噪声。
    """
    preds = []
    inputs = {"ppg_in": ppg, "scal_in": scal}
    for _ in range(n_samples):
        # training=True 让 Dropout 开启
        y = model(inputs, training=True).numpy().squeeze()
        preds.append(y)
    arr = np.stack(preds, axis=0)  # (N, B)
    return arr.mean(axis=0), arr.std(axis=0), arr


# ── 校准评估 ──────────────────────────────────────────────────────────


def calibration_metrics(y_true, y_pred_mean, y_pred_std):
    """评估不确定性是否 calibrated.

    1. abs_error vs std 的 Pearson 相关 -- 应该 > 0 (高 std 对应高误差)
    2. 95% CI coverage -- y_true 落在 [mean - 1.96*std, mean + 1.96*std] 的比例,
       理想 ~95%。常见偏低 (~70%) 因为 Dropout 倾向低估方差。
    3. 按 std 分箱, 看高/低 std 桶的 MAE 是否单调
    """
    abs_err = np.abs(y_pred_mean - y_true)

    # 1. abs_err vs std 相关
    if abs_err.std() < 1e-6 or y_pred_std.std() < 1e-6:
        corr = float("nan")
    else:
        corr = float(np.corrcoef(abs_err, y_pred_std)[0, 1])

    # 2. 95% CI coverage (1.96 std)
    lo = y_pred_mean - 1.96 * y_pred_std
    hi = y_pred_mean + 1.96 * y_pred_std
    coverage_95 = float(np.mean((y_true >= lo) & (y_true <= hi)))

    # 50% CI (0.674 std)
    lo50 = y_pred_mean - 0.674 * y_pred_std
    hi50 = y_pred_mean + 0.674 * y_pred_std
    coverage_50 = float(np.mean((y_true >= lo50) & (y_true <= hi50)))

    # 3. 按 std 分 3 桶, 看 MAE 单调性
    sorted_idx = np.argsort(y_pred_std)
    n = len(sorted_idx)
    low_idx = sorted_idx[:n // 3]
    mid_idx = sorted_idx[n // 3 : 2 * n // 3]
    hi_idx = sorted_idx[2 * n // 3 :]
    mae_low = float(np.mean(abs_err[low_idx]))
    mae_mid = float(np.mean(abs_err[mid_idx]))
    mae_hi = float(np.mean(abs_err[hi_idx]))

    return {
        "abs_err_vs_std_corr": corr,
        "coverage_95_pct": coverage_95 * 100,
        "coverage_50_pct": coverage_50 * 100,
        "mae_low_std_bucket": mae_low,
        "mae_mid_std_bucket": mae_mid,
        "mae_hi_std_bucket": mae_hi,
        "monotonic": mae_low < mae_mid < mae_hi,
        "std_mean": float(y_pred_std.mean()),
        "std_min": float(y_pred_std.min()),
        "std_max": float(y_pred_std.max()),
    }


# ── 主流程 ────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ckpt", type=Path, default=None,
                        help="checkpoint .h5 path")
    parser.add_argument("--n-samples", type=int, default=30,
                        help="MC dropout forward passes (default 30)")
    parser.add_argument("--auto", action="store_true",
                        help="auto-select latest best_fold*_seed*.h5")
    args = parser.parse_args()

    _ensure_deps()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # 选 checkpoint
    if args.ckpt is None or args.auto:
        cands = sorted(CKPT_DIR.glob("best_fold*_seed*.h5"))
        if not cands:
            cands = sorted(CKPT_DIR.glob("best_fold*.h5"))
        if not cands:
            print("[err] no checkpoint found")
            sys.exit(2)
        args.ckpt = cands[0]
    if not args.ckpt.exists():
        print(f"[err] ckpt not exists: {args.ckpt}")
        sys.exit(2)
    print(f"[ckpt] {args.ckpt.name}")

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

    # 用此 ckpt 对应 fold 的 val 集 (避免泄漏)
    import re
    m = re.search(r"fold(\d+)", args.ckpt.name)
    if m:
        fi = int(m.group(1))
        from sklearn.model_selection import GroupKFold
        splits = list(GroupKFold(n_splits=5).split(np.arange(len(y)), groups=sid))
        _, val_idx = splits[fi]
        print(f"[fold {fi}] eval on val ({len(val_idx)} samples)")
        X_ppg_eval = X_ppg[val_idx]
        X_scal_eval = X_scal[val_idx]
        y_eval = y[val_idx]
    else:
        print("[warn] ckpt name lacks fold info; eval on full (with leak)")
        X_ppg_eval = X_ppg
        X_scal_eval = X_scal
        y_eval = y

    # 加载模型
    print(f"[load] {args.ckpt}")
    model = tf.keras.models.load_model(str(args.ckpt), compile=False)

    # MC Dropout 推理
    print(f"[mc-dropout] N={args.n_samples} forward passes...")
    y_mean, y_std, _ = mc_dropout_predict(model, X_ppg_eval, X_scal_eval,
                                          n_samples=args.n_samples)
    print(f"[results] pred mean range [{y_mean.min():.3f}, {y_mean.max():.3f}]")
    print(f"          pred std  range [{y_std.min():.4f}, {y_std.max():.4f}]")
    print(f"          mean +/- std avg: {y_mean.mean():.3f} +/- {y_std.mean():.4f}")

    # 校准评估
    cal = calibration_metrics(y_eval, y_mean, y_std)

    print("\n" + "=" * 72)
    print(" Calibration Metrics")
    print("=" * 72)
    print(f"  abs_error vs std corr   : {cal['abs_err_vs_std_corr']:+.3f}  "
          f"({'OK (high std -> high err)' if cal['abs_err_vs_std_corr'] > 0.2 else 'WEAK / no calibration'})")
    print(f"  95% CI coverage         : {cal['coverage_95_pct']:.1f}%  "
          f"(ideal ~95, Dropout 常 60-80)")
    print(f"  50% CI coverage         : {cal['coverage_50_pct']:.1f}%  "
          f"(ideal ~50)")
    print(f"  MAE by std bucket       : low={cal['mae_low_std_bucket']:.3f}  "
          f"mid={cal['mae_mid_std_bucket']:.3f}  hi={cal['mae_hi_std_bucket']:.3f}")
    print(f"  monotonic (low<mid<hi)  : {cal['monotonic']}")
    print(f"  std stats               : mean={cal['std_mean']:.4f}  "
          f"range=[{cal['std_min']:.4f}, {cal['std_max']:.4f}]")

    # 保存
    report = {
        "experiment": "P3 C2 MC Dropout Uncertainty",
        "ckpt": str(args.ckpt),
        "n_samples": args.n_samples,
        "n_eval": int(len(y_eval)),
        "pred_mean_stats": {
            "min": float(y_mean.min()), "max": float(y_mean.max()),
            "mean": float(y_mean.mean()), "std": float(y_mean.std()),
        },
        "pred_std_stats": {
            "min": float(y_std.min()), "max": float(y_std.max()),
            "mean": float(y_std.mean()),
        },
        "calibration": cal,
    }
    out = OUT_DIR / "uncertainty_report.json"
    out.write_text(json.dumps(report, indent=2, ensure_ascii=False, default=float))
    np.save(OUT_DIR / "mc_pred_mean.npy", y_mean)
    np.save(OUT_DIR / "mc_pred_std.npy", y_std)
    np.save(OUT_DIR / "mc_y_true.npy", y_eval)
    print(f"\n[save] {out}")
    print(f"[save] mc_pred_{{mean,std}}.npy + mc_y_true.npy -> {OUT_DIR}")

    # M55 部署提示
    print("\n" + "=" * 72)
    print(" M55 Deployment Guidance")
    print("=" * 72)
    print(f"  - 推理 {args.n_samples} 次, 单次 ~5ms (NPU), 总耗时 ~{args.n_samples * 5}ms")
    print(f"  - UI 显示: blood glucose = {y_mean.mean():.2f} +/- {y_std.mean()*1.96:.2f} mmol/L (95% CI)")
    if cal['abs_err_vs_std_corr'] > 0.2:
        print(f"  - [OK] std 与误差正相关 (corr={cal['abs_err_vs_std_corr']:+.3f}), 可信展示")
    else:
        print(f"  - [WARN] std 与误差相关弱 (corr={cal['abs_err_vs_std_corr']:+.3f}), 仅作展示不作判断依据")


if __name__ == "__main__":
    main()
